# What is skimped, and what closing it takes

An honest inventory of every place the tree substitutes, shortens or leaves
something unmeasured. `docs/parity.md` covers the README's rows; this covers the
gaps *inside* rows and the things no row names.

Rule for reading it: an item appears here because someone checked, not because
it was suspected.

## Substitutions

| thing | what ships instead | who knows | closing it |
|---|---|---|---|
| embedded CPython (`python` row) | `/py` runs host `python3 -I` in the worker (`src/core/turn.c:727`), 15 s / 64 KiB, approval required | `docs/parity.md:36`, `docs/protocol.md:215`, journal 54 | in progress: frozen-stdlib `wasm32-wasip1` build, ~29 MiB, no preopen (journal 173) |
| judge model backend | rules decide, and the score is wired: `judge.score` posts to `$WARD_JUDGE_ENDPOINT/v1/systemone` and the guest reads it as input | journal 172, `gate_judge` | needs a native decision model behind the endpoint — `/v1/systemone` answers only for models whose `output_modalities` include `decisions`, and the box's `d1-2B-Q8_0-Decision` is not verified against this build |
| tokenizer tenant | token counts come from provider `timings` when present; otherwise a byte estimate flagged by `tokens_estimated` | journal, `src/daemon/server.c:653` | build the tenant; until then the flag is the contract |

## Partial, by design or by omission

| thing | state | closing it |
|---|---|---|
| cost accounting | counts were measured but **pricing used bytes/4**; fixed 2026-11-03 to price from measured tokens when present (`src/daemon/server.c:656`) | done; the byte fallback remains for providers without `timings` |
| compaction and the prefix | `session.compact` still rewrites the head in place, which discards the cache the prefix stability work exists to protect (`docs/local-compat.md:50`) | branch by default through `wd_session_fork`; additive events |
| module state handover | `ward_state_migrate` exists and is called (`src/wasm/host.c:734`, `:697`, `:717`), but **no shipped guest carries state**, so the acceptance is core-to-core | a stateful first-party guest, or the conformance guest as the only proof |
| job output overflow | the ring's accounting is unit-tested; **no gate drives it past 64 KiB through a real turn**, because the fixture does not stream that much (journal 165) | a provider fixture that streams over the cap |
| session search reindex | built and gated (`gate_reindex`): start a daemon, prompt, stop, delete `search.db`, start again and the old session is searchable | done |
| estate.card | **gated** (`estate_test`): a fixture unix socket serves the busy and idle documents, and the reader is asserted for holders, `since_ms`, a `loading` slot and an empty holder list. The earlier "hang" was a build taking longer than the shell timeout, and the read failed for a real reason: `CURLOPT_UNIX_SOCKET_PATH` was only set on the streaming path, so the plain GET went to TCP and refused | done |
| MCP, web search, recall | gates run **stub binaries** (`WARD_MCP_BIN`, `WARD_SEARCH_BIN`, `WARD_RECALL_BIN`); the wiring is proven, the tools are not | deliberate: those tools need host configuration the suite must not own |

## Client surfaces that are compiled but never exercised

| surface | caveat |
|---|---|
| desktop pairing dialog, settings section, jobs dialog | no gate clicks a dialog; the methods they call are gated instead |
| desktop jobs roster | empty in the fixture, so the roster and follow viewer have never met a real running job |
| Android QUIC/pairing | transport works (proven live over QUIC on desktop); the Android side has no QUIC stack, so `adb reverse` TCP is the supported door |
| Android module switch | `module.list`/`module.enable` are not surfaced; no plugin screen exists |

## Deliberate design, not gaps

No Windows or macOS, no web UI, no ACP, no FTS table, no 0-RTT, no emulator.
`README.md` says why; they are not to be "fixed".

## How to keep this honest

Every row here was produced by reading the tree or by a gate that failed. When
one closes, move it out rather than leaving a stale line — journal entry 171 is
the example: it corrects an earlier claim that `ward_state_migrate` did not
exist, because by the time it was re-read, it did.

## core audit, 2026-11-03

A read-only audit of `src/core/`, `src/proto/` and `src/cli/` (three subagents, none of which built or ran anything) found
one class of bug we had already shipped once: a pointer whose owner had freed or rewritten it. Fixed in the same pass:
the `turn.timings` map leak (turn.c, both emit sites), the `session.upgraded` record naming the new version as its `from`
value (session.c), and `wd_jobring_data` returning a rotated buffer once the ring wrapped (jobring.c, which fed both
`job.output` and the search index). Its test still passes.

Left open, in the auditor's order of severity:

- `deliver_steers` (turn.c) appends a second `prompt` key to an exact-sized map, so queued steering is dropped; folding it
  into the payload is the fix.
- `wd_put` (codec.c) drops `cbor_map_add`'s result, which is what made the above silent.
- `wd_session_replay` (session.c) returns an emitted count when the callback stopped or the log was truncated.
- `emit_provider_error`'s `reason` (turn.c) and `turn->steers[]` (turn.c teardown) leak.
- the session header mismatch path (session.c) leaks `cwd` and `model`.
- `src/cli/main.c` reads floats and bools through the wrong predicates — latent, since today's daemon sends the expected types.

The audit found no double free, no use-after-free, no mutate-while-iterating defect and no sqlite or fd lifetime bug in those
directories.

## wasm, net and modules audit, 2026-11-03

Scope correction from the auditor: `src/wasm/` holds only `host.c`/`host.h`; limits and migration live there, the ABI codec is
`modules/abi/wd_abi.h`, and **there is no instance pool** — every turn builds its own instances and frees them once. These
are unverified findings; none were applied.

- `modules/abi/wd_abi.h:133` **critical** — `r->at + len > r->len` wraps in uint32, so a text header of 0xFFFFFFFF passes the
  bound and `wd_r_text`'s `n + 1 > cap` wraps too. Reachability unconfirmed (core never emits such a length) but this is every
  guest's only defence. Fix: `len > r->len - r->at`, `n >= cap`.
- `src/wasm/host.c:604,626,652,748` **high** — the 8-byte result descriptor is never zeroed, so a guest that returns early
  replays the previous call's descriptor as this call's result. Fix: write 0xFFFFFFFF on alloc and treat it as "no output".
- `modules/*/ward_alloc` + `src/wasm/host.c:572` **high** — guests signal exhaustion with 0, which is a legal wasm address and
  is accepted, so an oversized input is memcpy'd over the guest's low memory. Fix: reject `at == 0`.
- `modules/provider/provider.c:271` **high** — history truncates at 32 by the loop condition without consuming the rest, so the
  reader is left mid-array and the call writes no reply. Fix: loop to `hn`, guard only the store.
- `modules/provider/provider.c:468` **high** — a request over the 24 KiB reply buffer returns with no reply and no error.
- `src/net/http.c:174` **high** — an interim `100 Continue` is latched as the final status for streamed turns. Fix: only latch
  `>= 200`.
- `src/net/http.c:112,71,78,120` **medium** — `*err` written without a NULL guard while its sibling guards it.
- `src/net/http.c:316` **medium** — `wd_http_job_finish` blocks the event loop on the pipe with no deadline.
- `src/wasm/host.c:151` **medium** — stdout capture doubles without a ceiling.
- `modules/{tools,code,policy,judge,skills,python}.c` **medium** — no descriptor written when an answer does not fit, which the
  host defect above turns into a stale replay.
- `src/net/estate.c:19,24,37,46,84` **medium** — the scanner takes the next quote after a key (`"group":null` adopts the next
  token), never skips whitespace after `[`/`,` so spaced holders read as none, trusts `strtoull` on raw bytes, truncates names
  silently at 16/32, and interpolates the group into the URL unencoded.
- `src/net/http.c:44` **medium** — the unix-socket path is a process global holding a borrowed pointer.
- `modules/policy/policy.c:31` **medium** — rule strings live in 8-byte buffers, so a longer rule yields no decision at all.
- `modules/schedule/schedule.c:90` **medium** — `from_ms` accumulates digits into uint64 unchecked.
