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
one class of bug we had already shipped once: a pointer whose owner had freed or rewritten it. The first pass fixed the
`turn.timings` map leak (turn.c, both emit sites), the `session.upgraded` record naming the new version as its `from` value
(session.c), and `wd_jobring_data` returning a rotated buffer once the ring wrapped (jobring.c, which fed both `job.output`
and the search index).

The second pass closed every remaining finding but one:

- `deliver_steers` now merges only into a payload that carries a string `prompt`, and `wd_put` replaces an existing key
  rather than appending a duplicate. A map that is genuinely full prints to stderr instead of dropping the pair in silence.
  `gate_native` drives steering, but no test asserts the merged text, so the merge itself is read, not gated.
- `emit_provider_error`'s `reason`, the leftover `turn->steers[]`, and the session header's `cwd`/`model` on an id mismatch
  are freed. `src/cli/main.c` uses `cbor_is_float` and `wd_bool_of`.
- `wd_session_replay` still returns an emitted count for a truncated log. Returning -1 would make a crash-torn session
  unopenable while its header is intact, so truncation needs its own out-parameter; that is an API decision, not a patch.

The audit found no double free, no use-after-free, no mutate-while-iterating defect and no sqlite or fd lifetime bug in those
directories.

## wasm, net and modules audit, 2026-11-03

Scope correction from the auditor: `src/wasm/` holds only `host.c`/`host.h`; limits and migration live there, the ABI codec is
`modules/abi/wd_abi.h`, and **there is no instance pool** — every turn builds its own instances and frees them once. These
were unverified when written; the second pass applied the high findings and left the medium ones:

Applied:

- `modules/abi/wd_abi.h` **critical** — bounds are subtraction-based and `wd_r_text` rejects `n >= cap` (commit `3da3dd1`;
  `abi_test` segfaulted before it).
- `src/wasm/host.c` **high** — the result descriptor is poisoned with `0xFFFFFFFF` before every guest call and `read_pair`
  reports "the module produced no result" instead of replaying a stale descriptor. `alloc_guest` now rejects a null address.
- `modules/provider/provider.c:271` **high** — the history loop consumes every entry and caps only the store, so the reader
  is no longer left mid-array after 32 entries.
- `src/net/http.c:174` **high** — the streamed header callback ignores statuses below 200, so `100 Continue` is not latched
  as the final status.
- `src/net/estate.c` **medium** — the scanner requires a quoted value after the key (so `"group":null` fails instead of
  adopting the next token), skips whitespace around array entries, validates `since_ms` digits, rejects over-long names
  instead of truncating, and percent-encodes the group. `estate_test` now covers spaced JSON and a null group.

Still open, all **medium**:

- `modules/provider/provider.c:468` — a request over the 24 KiB reply buffer now fails as "no result" at the host rather
  than replaying a stale descriptor, but the turn still fails with no explanation from the guest.
- `src/net/http.c:112,71,78,120` — `*err` written without a NULL guard while its sibling guards it.
- `src/net/http.c:316` — `wd_http_job_finish` blocks the event loop on the pipe with no deadline.
- `src/wasm/host.c:151` — stdout capture doubles without a ceiling.
- `modules/{tools,code,policy,judge,skills,python}.c` — no descriptor written when an answer does not fit; the host now
  reports it, but the guest still gives no reason.
- `src/net/http.c:44` — the unix-socket path is a process global holding a borrowed pointer.
- `modules/policy/policy.c:31` — rule strings live in 8-byte buffers, so a longer rule yields no decision at all.
- `modules/schedule/schedule.c:90` — `from_ms` accumulates digits into uint64 unchecked.

## daemon audit, 2026-11-03

Verified against `6f023cd` (4827 lines). The shipped use-after-free at `:3377` is confirmed fixed: `err` is strdup'd before
`run_after` takes and frees it. The second pass applied every finding except three:

Applied:

- the ten `conn_send_error` sites named by the auditor, plus four it did not list (`:1812` prompt preview, three attachment
  error paths), and the upgrade failure path, now call `conn_arm` before returning — fourteen in all. No gate asserts that an
  error frame reaches the client without a follow-up request, so this is read, not gated.
- `job.follow`'s capacity is `3 + (pushed ? 1 : 0)`, so `dropped` is present in a successful reply.
- `run_new` frees the 64 KiB ring on every early exit and releases the device lock on the retry path.
- `session.set_model`'s unused map, `module.enable`'s empty-name `name`, and `run_after`'s pending-path `err` are freed.

Still open:

- `job.follow` and `run_free`'s indexer still trust `wd_jobring_data()` to begin at the live bytes. `jobring_append` now
  rebases, so that holds, but a `wd_jobring_copy` accessor would remove the dependency.
- the `:3392` parked-run/device-lock path looks unreachable today and was left alone; the two `cbor_array_push` OOM leaks
  are untouched.
- `resolve_model_id` returns the first model whose display name matches, so two catalogue entries sharing a name resolve
  silently to the first.

Nothing found for dangling pointers, libcbor ownership, error strings outliving their frame, or iteration while mutating.
