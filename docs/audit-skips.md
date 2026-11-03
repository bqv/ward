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
| judge model backend | rules decide; `modules/judge/judge.c` is rule-shaped, nothing calls Kev | parity `judge` row, journal 51-55 | decided: `judge.score` capability, consulted only when the rule allows and the tool is write/edit/exec (journal 172); unwired |
| tokenizer tenant | token counts come from provider `timings` when present; otherwise a byte estimate flagged by `tokens_estimated` | journal, `src/daemon/server.c:653` | build the tenant; until then the flag is the contract |

## Partial, by design or by omission

| thing | state | closing it |
|---|---|---|
| cost accounting | counts were measured but **pricing used bytes/4**; fixed 2026-11-03 to price from measured tokens when present (`src/daemon/server.c:656`) | done; the byte fallback remains for providers without `timings` |
| compaction and the prefix | `session.compact` still rewrites the head in place, which discards the cache the prefix stability work exists to protect (`docs/local-compat.md:50`) | branch by default through `wd_session_fork`; additive events |
| module state handover | `ward_state_migrate` exists and is called (`src/wasm/host.c:734`, `:697`, `:717`), but **no shipped guest carries state**, so the acceptance is core-to-core | a stateful first-party guest, or the conformance guest as the only proof |
| job output overflow | the ring's accounting is unit-tested; **no gate drives it past 64 KiB through a real turn**, because the fixture does not stream that much (journal 165) | a provider fixture that streams over the cap |
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
