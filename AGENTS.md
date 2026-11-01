# Working in this repo

A coding agent harness: a small C core that owns I/O and enforcement, and a
reloadable WebAssembly layer that owns every decision. Binary protocol over a
Unix socket; no web UI.

**Status: design only.** The repository holds one file of prose — `README.md` —
which is the specification and the only source of truth here. There is no
`meson.build`, no `src/`, and no `docs/protocol.md` yet. Do not write code
against an imagined skeleton; if the work is not the first two weeks below, the
first task is nearly always to extend `README.md` and let the design settle.

`README.md` is the contract and the parity table is its spine. When this file and
`README.md` disagree, `README.md` wins and this file gets fixed.

## The one rule that decides everything

For anything new, ask **does it own authority, or does it make a decision?**

- **Authority** — a file descriptor, a socket, a child process, a durable
  invariant, a capability check, a version registry — belongs in core.
- **A decision** — what to say, what to run, how to rank, when to fire — belongs
  in a module.

Core is allowed exactly: the event loop, protocol server, capability broker,
Wasmtime host and registry, store, PTY and process spawning, config, codecs.
No prompt text, no tool logic, no provider quirks, no ranking, no policy rules,
no cron arithmetic. Budget: **core stays under roughly 8,000 lines**, and an
addition that breaks the budget must delete something or prove it is authority.
Three tenants are on notice (tokenizer, judge, recall); the README says why.

## Before the first line of code

The first two weeks, in order, are fixed by the README:

1. `meson.build` and `meson_options.txt`, dependencies pinned, CI with ASan,
   UBSan, TSan and `clang --analyze` from day one.
2. `docs/protocol.md` — **freeze the frame table and the method list before
   writing the codec.** Method names, event kinds and error codes are a wire
   format; changing them after a client exists means a version bump.
3. `src/proto`: frame codec, CBOR wrapper, fuzz harness.
4. `harness up`: socket, handshake, `session.create`, and a CLI client printing
   events.
5. The SSE client, before anything touches a model. Fuzz the parser before the
   provider client exists.

M0's acceptance is 1,000 round-trips/s over UDS and a fuzz-clean decoder. A
gate that is not measured is not met.

## Build

C11 and Meson, debug by default. `meson setup build -Dbuildtype=debug
-Dguest_profile=debug && meson compile -C build`. Two profiles, both Meson
options rather than edited build files: debug compiles guests at `-O0 -g` and
keeps DWARF; release is `-O2` and strips.

Dependencies resolve through `dependency()` and pkg-config: Wasmtime C API,
ngtcp2, OpenSSL, tree-sitter, libcurl, sqlite3, libcbor, yyjson, zstd. Guests:
`clang --target=wasm32-wasip1` against wasi-sdk for C and C++, Zig for Zig (pin
the version — the language still breaks between releases), a thin C module
embedding Janet for policy.

One build at a time, at the top level. A subagent edits and reasons; the
coordinating session runs the build and the suite and feeds the output back.
Never build release or `clean` on a guess.

## Layout

`src/core/` loop, sessions, capabilities, scheduler · `src/proto/` frames, CBOR,
method table · `src/wasm/` registry, instance pool, migration, ABI ·
`src/daemon/` socket, auth, subscriptions · `src/cli/` command line, headless ·
`modules/<name>/` first-party guests · `clients/` test client, then `dsh-native`
· `tests/` unit, golden transcripts, fuzz corpus · `docs/`.

Every guest has a manifest with `capabilities`, and the host refuses to
instantiate one whose capabilities are not granted. Adding a capability is a
core change and therefore needs the authority argument above.

## Traps already in the design

These are decisions, not open questions; a change to one is a design change.

- **Wasm cannot pause an instance mid-import.** A tool call is a *return* from
  the driver, re-entered with `{tool_result, ...}` — never a blocking host call.
  Anything that assumes otherwise will be rewritten.
- **A CPython heap cannot be exported.** The `python` guest lives exactly one
  turn and has nothing to migrate; anything that must outlive it goes through
  `store`.
- **`engine` has an empty import section**, enforced at load. It is the pure
  transition function CBMC checks. It does not grow capabilities.
- **`engine.call` is the only module-to-module call**, core-mediated, granted
  only to `driver`. Every other ABI name is a core capability that core may
  dispatch into a module.
- **Approvals are core state**, keyed `(session, call-id, epoch)`, first valid
  answer wins, and a late-attaching client still sees the request. A module
  reload cannot lose one, and the worker does not fork until resolution.
- **Fail closed.** Judge: `max(rule, judge)`, so it can raise `allow` to `ask`
  but never lower a `deny`; timeout or no model falls back to the rule, and an
  unanswerable `ask` is `deny`.
- **No 0-RTT.** The session is 1-RTT; it costs a reconnect round trip and
  removes a replay surface.
- **No FTS table.** `rg` decompresses a log on demand; if that gets slow the
  answer is a plain-text sidecar cache, not an index in SQLite.
- **Do not reimplement syscall-heavy tools in WASM.** Grep runs in the worker's
  file broker on top of `rg`; the module chooses paths, patterns and shaping.

## Rules for working here

- `master`, never `main`. English (UK) in every file, message and identifier
  comment. No web UI, no JavaScript shim, no Windows or macOS, no ACP in v1 —
  those are out of scope by decree, not by difficulty.
- No comments in code. If a stretch needs one, rework it.
- No `.sh` files anywhere. Helper scripts are execline, or bash when the task is
  genuinely bash-shaped, as an executable with no extension. No `sleep`-polling
  loops; wait for the event.
- Commits: author the owner, `bqv <822863+bqv@users.noreply.github.com>`;
  committer names the model, `<model> <agent@deepseek-harness>`. Never push,
  open an issue or comment anywhere on the owner's behalf without an explicit
  one-off OK for that text.
- Project notes belong here, in the repository — never in `~/ref`.
- **Licence is an open decision.** Nothing has been chosen yet; a fresh
  repository defaults to The Individual Use Licence (IUL-1.0.0) or MPL-2.0, not
  MIT. Do not paste a licence in as a side effect of another change.
- Say when something is a guess rather than a measurement. The parity table is
  the scope contract; anything not in it waits.
