# Parity audit

`README.md`'s parity table is the scope contract. This audit walks it row by
row and says what the tree actually does, with the evidence that proves it. The
rule applied throughout: **a row is `delivered` only when a gate exercises it**;
anything proven by reading alone, or by a fixture that does not cover the
behaviour, is marked `partial`.

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
| web search | M4 | **partial** | the capability exists in core and the `skills` module chooses; no gate runs `srchdsh` because the binary is host configuration |
| MCP client | M4 | **missing** | no module, no method, no journal entry |
| terminal and PTY | M4 | delivered | `pty_test` |
| attachments and uploads | M4 | delivered | `gate_tool` asserts `[attach]`; `attachment.add`/`list` |
| module reload and migration | M5 | delivered | `modstate_test`, `gate_tool` reload assertions |
| native client | M6 | delivered | `gate_native` — `ward-native` drives create, prompt and resume over the socket |
| Android client | M6 | **missing** | nothing exists; `docs/m6.md` scopes two candidate shapes |
| python eval | M4 | **substituted** | named as a substitution in the journal; no embedded CPython |
| recall | M4 | delivered | `recall` module with a stubbed binary in `gate_tool` |
| judge | M4 | **partial** | rules decide; the model backend is the substitution named in the journal |

## What this audit says about the project

Two rows are missing outright (MCP, Android), three are partial for reasons the
journal already names, and one is a substitution. Everything else is exercised
by a gated binary that fails when the behaviour changes.

The gap worth naming first is **MCP**: it is a row in the contract with nothing
behind it, and unlike Android it needs no device — just a client that speaks the
protocol to a server over stdio. It is the cheapest missing row to close.

## Local-model compatibility

`docs/local-compat.md` carries the second half of the audit: the eight items of
that note, each marked with what was measured. Its own rule is stronger than the
one here — an item is not closed until the *effect* is observed, not just the
code path.
