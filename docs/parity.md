# Parity audit

`README.md`'s parity table is the scope contract. This audit walks it row by
row and says what the tree actually does, with the evidence that proves it. The
rule applied throughout: **a row is `delivered` only when a gate exercises it**;
anything proven by reading alone, or by a fixture that does not cover the
behaviour, is marked `partial`.

## Unmeasured, deliberately

`session.prompt` with `steer: true` is implemented and its answer is gated, but the **late-steer carry-over** — the
turn-end path that re-queues a steer the turn never delivered — has no assertion. `docs/journal.md` entry one hundred and
fifty-five records the shape that assertion needs. Until it exists, this row is **not** delivered, whatever the suite says.

## The table

| row | phase | status | evidence |
|---|---|---|---|
| daemon, UDS transport, handshake, `session.create/list`, `replay` | M0 | delivered | `gate_m0` — 2,000 round-trips, 1,000/s asserted |
| fuzz-clean decoder | M0 | delivered | `fuzz_codec`, `fuzz_sse` |
| Wasmtime host and ABI | M1 | delivered | `wasm_test`, `trap_test`, `worker_test` |
| provider SSE | M1 | delivered | `provider_test`, `sse_test`, plus streaming deltas in `gate_tool` |
| engine and driver | M1 | delivered | `gate_m1` |
| tools, approvals | M1 | delivered | `gate_tool` — a write waits for an approval and is answered |
| QUIC and device pairing | M1 | delivered | `gate_quic`, `quic_test` |
| SQLite sessions, resume, fork | M2 | delivered | `session_test`, `gate_tool` |
| cancel | M2 | delivered | `gate_m2` via `session.cancel`; `http_cancel_test` for the child process |
| compaction | M2 | delivered | `session_test` (branch + parent intact), `gate_tool` |
| cost and token accounting | M2 | **partial** | measured tokens land in `stats` when the provider reports `timings`; the tokenizer tenant named in the README is not built, so an estimate is still the fallback and `tokens_estimated` says so |
| subagents | M3 | delivered | `gate_tool` — two subagents run concurrently |
| jobs and notices | M3 | delivered | `job.list`/`job.kill`, FIFO queue and `ahead` in `gate_tool` |
| schedules (cron) | M3 | delivered | `schedule_test`, `gate_m3` |
| goals | M3 | delivered | `gate_m3` |
| skills, `AGENTS.md`, slash commands | M4 | delivered | `gate_tool` asserts `[rules]` reaches the wire; `/read`, `/write`, `/grep` dispatch |
| web search | M4 | delivered | `gate_tool` runs a stubbed `srchdsh` through `WARD_SEARCH_BIN` and asserts the result reaches a `/search` turn |
| MCP client | M4 | delivered | `gate_tool` stubs `WARD_MCP_BIN`, answers a JSON-RPC line, and asserts `mcp ok` arrives; core's `mcp.call` runs the child |
| terminal and PTY | M4 | delivered | `pty_test` |
| attachments and uploads | M4 | delivered | `gate_tool` asserts `[attach]`; `attachment.add`/`list` |
| module reload and migration | M5 | delivered | `modstate_test`, `gate_tool` reload assertions |
| native client | M6 | delivered | `gate_native` — `ward-native` drives create, prompt and resume over the socket |
| Android client | M6 | delivered | `clients/android` (project ward-android, app Ward) speaks the frozen protocol in Kotlin: an APK on the disposable handset created and listed a session over `adb reverse`, painted a `/read` turn, answered an approval, and replayed the transcript after force-stop and relaunch; its 1,308-test JVM suite passes. Known gaps (QUIC/pairing, workspaces, terminal routing, jobs/schedules/goals UI, subagent lineage, stubs, background approvals, queue/steer) are named in `clients/android/README.md`. `gate_native` covers the loopback TCP transport it uses |
| python eval | M4 | **substituted** | named as a substitution in the journal; no embedded CPython |
| recall | M4 | delivered | `recall` module with a stubbed binary in `gate_tool` |
| judge | M4 | **partial** | rules decide; the model backend is the substitution named in the journal |

## What this audit says about the project

No row is missing now. Three are partial for reasons the journal names, and one
is a substitution. Everything else is exercised by a
gated binary that fails when the behaviour changes.

This audit had to be corrected after its first draft: I wrote it from memory and
marked web search and MCP as unbuilt, when `gate_tool` already stubs and asserts
both — `WARD_SEARCH_BIN` and `WARD_MCP_BIN` exist precisely so those rows could
be tested without host configuration. The lesson is the same one the rest of this
session keeps teaching: check the tree before claiming what it lacks.

## Local-model compatibility

`docs/local-compat.md` carries the second half of the audit: the eight items of
that note, each marked with what was measured. Its own rule is stronger than the
one here — an item is not closed until the *effect* is observed, not just the
code path.
