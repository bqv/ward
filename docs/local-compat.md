# Local compatibility: making ward fit llama.cpp on one GPU

ward assumes a cloud provider: a large window, cheap prefill and a server that accepts OpenAI's whole parameter surface. The owner's box is a 27B Q4 model, 32–65k of context, 40 s of prefill for 26k tokens and one RX 7900 XT shared by every subagent. These eight changes make ward accurate against local llama.cpp routers. Uncertain claims are guesses.

## Status

Landed: the catalog carries `endpoint`, `key`, `max_output`, `device`, `slots` and `stop`; the request names the
catalog's model and bounds its length; a remote endpoint is admitted only when a model names one; `model.probe`
reads `n_ctx` from the server and the prompt budget follows it; the head is frozen once and history is never
silently trimmed; the driver no longer stamps the clock and the time is a durable tail event; compaction branches
instead of rewriting; the error taxonomy covers the provider failures a local stack produces; cancel kills the
curl child; `turn.timings` records the router's own numbers and they are summed into stats; `turn.state` names the
wait; the queue is FIFO with `ahead`; a model's device has a slot pool and one queued turn per session; and the
provider reads a native `tool_calls` reply, and a reply asking for more than one call is refused rather than
half-run.

Streaming is in the tree and working — the helper pipes chunks, the daemon parses them with the existing SSE
parser and appends a durable `text.delta` per fragment, and the driver's own delta is suppressed so nothing is
repeated — but its *timing gate* is still missing, and the fake endpoint is what blocks it: the fixture emits a
single non-empty content fragment per reply, so a test cannot yet assert that a delta arrives before `turn.end`.

Still open: the streaming timing gate (fixture first), the one repair for a failed tool-call parse, and the
per-daemon limits of the device pool.

## 1. Model capabilities, not just context length

`models.toml` accepts six text keys (`id`, `provider`, `model`, `context`, `tags`, `price`; `src/core/models.c:46-65`), and `model.list` serves five of them — `price` is not served — as `{id, provider, model, context, tags}` (`src/daemon/server.c:316-325`, `docs/protocol.md:344`). The endpoint is not in the catalog; the daemon reads `WARD_ENDPOINT` (`src/core/turn.c:969`).

Extend `wd_model` (`src/core/models.h:12-19`), `set_field` and `model_to_cbor` with `max_output`, `tool_mode` (`native`/`grammar-only`/`none`), `reasoning_channel` (`reasoning_content`/`none`), `supports_images`, `prefix_cache`, `stop`, `endpoint`, `device`, `slots` and the sampling defaults from §8. New TOML and map keys are compatible (`docs/protocol.md:462-464`): no bump. Clients stop inferring from the model name, and the provider can refuse what the server would only 400 on — an image to a vision-less build, `tools` to a chat-only template, `max_tokens` above the window.

## 2. Prefix stability as the performance story

The head is whatever `history_with_context` prepends at the turn: rules re-read from `<cwd>/AGENTS.md` every prompt (`src/core/turn.c:1472-1527`), an attachment line rebuilt per run (`src/daemon/server.c:1785-1787`), then history trimmed from the front to a byte budget (`src/core/session.c:549-556`).

The timestamp is the sharpest bug. The driver stamps every provider prompt as `[YYYY-MM-DD HH:MM:SS TZ] hello` (`modules/driver/driver.c:443-467`; format at `src/core/turn.c:288-298`), but `session.prompted` records the raw text (`src/daemon/server.c:1932-1938`). Replay sends `hello` where the live request sent the stamped line, so the rebuilt history is never byte-identical and llama.cpp re-prefills from the first user turn.

- **Freeze the head.** Append one durable `session.head {rules, rules_hash, attachments}` on the first prompt and build the system block from it, never re-reading `AGENTS.md`; `session.set_rules` changes it and resets the cache.
- **Time in the tail, persisted.** Drop the driver stamp, emit `session.time {text}` before `session.prompted`, render it as `system`, and forward the same string; replay then reproduces the bytes (`docs/protocol.md:276-278` promises injection, not volatility).
- **Stop trimming.** Delete the front trim and the 8192-byte budgets (`src/daemon/server.c:1096`, `1929`, `1989`); overflow is an explicit compact or branch, never a silent drop.
- **Test it.** Assert body N+1 starts with body N; counting messages will not catch a moved byte.

Compaction breaks the prefix by design: `session.compacted` discards every turn and pushes the summary first (`src/core/session.c:483-499`), and the builder ignores `through`, so `docs/protocol.md:230-234`'s "only turns after `through`" is not what the code does. `wd_turn_compact` also summarises unbounded history (`src/core/turn.c:1664-1706`). Make `session.compact` branch by default through `wd_session_fork`: the summary heads a new session, the old keeps its cache. If in-place is required, add `cache_reset: true`, send `cache_prompt: false` next (guess) and compact only a tail window. Additive events, no bump. Never rewrite the head mid-session.

## 3. Context budget from the server, not a constant

Nothing probes the server. `model.context` is stored and served but never parsed (`src/core/models.h:15`, `src/daemon/server.c:322`); the only prompt-path budget is 8192 bytes (`src/daemon/server.c:1929`), smaller than one 26k-token prompt.

Add **`model.probe`**, additive and bump-free. Core performs a bounded loopback `GET /props` with a 2 s timeout (the existing `wd_http_request` supports GET, `src/net/http.c:73-78`), reuses `wd_http_loopback`, and answers `{id, n_ctx, n_slots, context_source}`. It reads `default_generation_settings.n_ctx` and `total_slots` (I am confident about those llama.cpp keys; a per-model `n_ctx` in the `models` metadata is a guess), cached per endpoint. Core has no JSON decoder (`Makefile:12`); a small key scanner like `modules/provider/provider.c:82-123` is enough.

Probe and cache are authority (core). The reserve arithmetic is a provider quirk, so the provider guest gains a `budget` phase: `{n_ctx, max_output, prompt_bytes} -> {input_budget, compact_at}`. The daemon asks before `wd_session_history`, passes the real budget, and triggers the existing compaction handler (`src/daemon/server.c:568-632`) at `compact_at`. When the server is silent, parse `context` (`32768`, `32k`) and report `context_source: "config"`; with neither, use 8192 and `"default"`. A changed `n_ctx` after a restart is a re-probe; an over-budget request is `context_exceeded` (§6).

## 4. Tool calling where local models break

There is no model-driven tool call today. The driver interprets slash commands (`modules/driver/driver.c:300-442`), the provider builds only `messages` (`modules/provider/provider.c:207-237`), and the parser looks only for `"content":"` (`modules/provider/provider.c:82-123`). The tools guest is a pure `{tool, args} -> {call, payload}` mapping (`modules/tools/tools.c:39`); the provider ABI is internal, so no protocol change.

- **Native.** One tool per turn: one `tools` entry, `tool_choice` naming it, `parallel_tool_calls: false`. Parse `choices[0].message.tool_calls[0].function.{name,arguments}` and `finish_reason == "tool_calls"`.
- **Grammar-only.** Send a GBNF `grammar`, or a JSON-schema `response_format` on builds that honour it (guess: both are build-dependent), constraining exactly `{"tool": "...", "args": {...}}`. Keep schemas flat — one required string field, under 1 KiB.
- **Lenient parse, one repair.** Accept `arguments` as an object or string, a bare object, and a fenced JSON block; strip prose. On failure re-ask once with a fixed "return only the JSON object" instruction and the parser error, then fail with `tool_parse` and a bounded excerpt. Never rewrite what the model said.
- **Multiple calls** are a `tool_parse` failure, not a silent first-only dispatch.

The parse point is a new `parse_completion` replacing the body branch of `ward_call` (`modules/provider/provider.c:193-203`), returning `{text, tool?, name?, arguments?, reasoning?, finish}`. `process_action`'s provider branch (`src/core/turn.c:1316-1339`) feeds it back, and `invoke_tool` (`modules/driver/driver.c:226-268`) stays the only place that mints an `invoke`.

## 5. Streaming and timeouts

`wd_sse` exists (`src/proto/sse.c`, fuzzed by `tests/fuzz_sse.c`) but the product never calls it. `src/net/http.c:21-32` buffers the whole body, the only timeout is 300 s (`src/core/turn.c:146`, `:1178`), and the provider emits one `text.delta` at the end (`modules/driver/driver.c:128-151`). The answer buffer is 2048 bytes (`modules/provider/provider.c:9`, `70-73`), so long answers truncate, and non-ASCII `\u` escapes become `?` (`modules/provider/provider.c:99-108`).

- **Stream properly.** The curl write callback feeds a pipe; the daemon feeds `wd_sse` per chunk and sends `{chunk}` to the provider, which gains a third phase returning `{text, done}`. It touches the provider ABI and `wd_run` epoll, but makes a 40 s prefill visible.
- **Partial UTF-8.** Carry 0–3 bytes at chunk boundaries, never emit invalid `text.delta`, and decode surrogate pairs.
- **`[DONE]`** ends the stream without waiting for curl.
- **Split the timeout.** First byte 120 s (a 40 s prefill with margin), inter-chunk idle 30 s, and a total cap. Emit `turn.start {state: "prefilling"}` (an added key) and clear it on first content.
- **Abort by killing the helper.** Closing the pipe does not cancel curl; `wd_http_job_free` blocks in `waitpid` (`src/net/http.c:205-212`) and `wd_turn_abort` calls it (`src/core/turn.c:1610-1613`), so cancel can hang for the timeout. Add `wd_http_job_cancel` (`SIGTERM`, `SIGKILL`, reap).
- **Record timings.** Extract `prompt_n`, `prompt_ms`, `predicted_n`, `predicted_ms` and `cache_n` from llama.cpp's `timings`, and append a durable `turn.timings` event; `stats.session` counts bytes only today (`src/daemon/server.c:459-485`). New keys and event kinds are additive, so no bump.

## 6. A distinct error taxonomy

The daemon has 13 error names (`src/proto/codec.h:18-33`, `docs/protocol.md:425-439`). A non-200 is not one: the provider parses only 200 (`modules/provider/provider.c:193-203`), so 400, 500 and 501 all become `{text: ""}` and an ordinary `turn.end`; a transport failure becomes "the provider request failed" (`src/core/turn.c:1641-1647`).

| code | name | local cause | what the client does |
|---|---|---|---|
| 14 | `provider_unavailable` | connection refused, router dead | restart llama |
| 15 | `model_limit` | VRAM margin or `--models-max` | free a slot |
| 16 | `endpoint_unsupported` | 501, e.g. embeddings on a chat model | do not retry |
| 17 | `context_exceeded` | 400, prompt over `n_ctx` | compact |
| 18 | `provider_timeout` | first-byte or idle budget expired | raise or shrink |
| 19 | `provider_rejected` | other 4xx: bad schema, `logit_bias` | fix the request |
| 20 | `tool_parse` | output not JSON after one repair | show raw text |

Emit the frozen, never-emitted `error` event (`docs/protocol.md:398-403`) with `{code, name, message, turn}`, then `turn.end {turn, aborted: true, reason}`.

**Version bump.** Adding an error name or a method is compatible under the frozen rule (`docs/protocol.md:462-464`), so strictly no bump, and I would not bump: it makes v1 peers unnegotiable (`docs/protocol.md:94-96`). The one contractual break is the meaning of success: today a provider failure ends normally with empty text; after this it ends `aborted` with an `error` event. If the owner wants that marked, use `WD_PROTOCOL_VERSION 2` (`src/proto/codec.h:14`); otherwise ship as v1.

## 7. Serialise per device

`max_runs` defaults to 8 with a hard cap of 16 (`src/daemon/server.c:2958-2962`, `:106`), the queue is global and LIFO (`src/daemon/server.c:1135-1136`, drained at `1113-1121`), and `job.list` shows `running`/`queued` but no device or depth (`src/daemon/server.c:1011-1037`). Subagents get their own runs (`src/daemon/server.c:1154-1189`), so the M3 "two subagents concurrently" gate is precisely two competing prefills on one GPU.

Add a per-device semaphore in core beside the run pool: `wd_device_pool`, keyed by the model's `device`, with `slots` from `models.toml`. Acquire before `net_submit`/`wd_http_start`, release on finish or abort. Queue per device, FIFO, at most one queued entry per session so one parent cannot starve another. Compaction uses the same provider path (`src/core/turn.c:1664`) and must take the same slot; judge and recall use the aux endpoint (`README.md:224-226`), a different device.

Observe it by adding `device` and `ahead` to each `job.list` entry and a top-level `devices: [{device, inflight, queued, slots}]`; expose `device` and `slots` in `model.list`. All are new keys, so no bump. The failure prevented: four subagents prefilling 26k on one 7900 XT finish later than in sequence, with VRAM the only limiter.

## 8. Small wins

- **Unix socket.** Add `endpoint = "unix:/run/llama.sock"` to `models.toml`. `wd_http_loopback` accepts only `http://127.0.0.1` and `http://localhost` (`src/net/http.c:34-43`); teach it `unix:` and set `CURLOPT_UNIX_SOCKET_PATH` in `wd_http_request`. `net_request` and `net_submit` (`src/core/turn.c:123-166`, `1158-1183`) only pass the endpoint through; no protocol change. Absolute path, no symlinks, loopback trust.
- **Sampling defaults.** The provider sends `{"model":"local","stream":true,"messages":[...]}` and nothing else (`modules/provider/provider.c:207-208`), so every run inherits llama.cpp's defaults. Send `temperature`, `top_p`, `top_k`, `min_p`, `repeat_penalty`, `seed`, `n_predict` and `stop` from the catalog.
- **No cloud-only parameters.** Build from a per-model allowlist; never send `logit_bias` unless the entry declares it. On llama.cpp it is refused on some builds and silently ignored on others. The choke point is `modules/provider/provider.c:207-237`.
- **Send the real model name.** `"model":"local"` is hardcoded; a router that routes by name will 400 or pick the wrong rung. Use the catalog's `model`.

## What I would do first

1. **Error taxonomy** (§6) — small, local to provider and `wd_turn_resume_http`; debugging becomes diagnosis.
2. **Catalog and `model.probe`** (§1, §3) — data plus one method; stops 8192 bytes discarding 26k.
3. **Prefix stability** (§2) — freeze the head, drop the stamp, stop trimming, test the prefix. This is the 40 s per turn.
4. **Streaming, abort and timings** (§5) — changes the provider ABI, but makes the wait visible.
5. **Per-device serialisation** (§7) — small, and stops subagents slowing each other.
6. **Tool calling** (§4) — largest and least certain.

Sampling and the unix socket ride along with step 2.
