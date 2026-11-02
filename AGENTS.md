# Working in this repo

A coding agent harness: a small C core that owns I/O and enforcement, and a
reloadable WebAssembly layer that owns every decision. Binary protocol over a
Unix socket; no web UI.

**Status: M5 complete, M6 open.** M0 to M5 are built and gated: the daemon, the
`AF_UNIX` transport, the handshake, sessions, durable events and replay; the Wasmtime
host, the provider, the engine/driver dispatcher, QUIC with device pairing; the store,
compaction, queue, checkpoints; subagents, jobs, schedules and goals; module versions,
drain and migration, reload and the code index. The daemon runs inside its own cgroup
(`docs/cgroup.md`), local-model compatibility is documented in `docs/local-compat.md`,
and M6 — a native client, then Android, then the parity audit — is scoped in
`docs/m6.md`. `docs/protocol.md` is a frozen contract: the frame layout, the method names
and the error names are a compatibility surface, so changing one is a version
bump, not an edit. `README.md` remains the design, its parity table is the scope contract, and the
design outranks this file; when they disagree, the README wins and this file gets
fixed.

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

## Where the work stands

M0 to M3 are complete and gated. M4 is nine rows of eleven: the judge's model backend and an
embedded CPython are the gaps, and both are named as substitutions in the journal. M5 has every
piece except the last: versions, reload, pinning, upgrade, handover between instances and
demotion are built and measured; a turn now resolves through its pins, but nothing persists a
guest's state across a turn, so there is nothing to hand over yet and the acceptance line is
"proven with a fixture" rather than "the session moved". The design for the remaining piece is
in the journal, entry seventy.

The history — every slice, every trap, and the resume notes for M5 — is in `docs/journal.md`.
Read it before changing anything that looks arbitrary.

## Build

C11 and GNU make, debug by default. `make -j4` builds into `out/`, and
`make test` builds the suite and runs it. Two profiles, make variables rather
than edited build files: `make GUEST_PROFILE=release` compiles guests at `-O2`,
and `make MODE=release` does the same for the host. Debug guests keep DWARF.

The guest toolchain is `/opt/wasi-sdk-34`, installed by `dev-util/wasi-sdk-bin`
from the bqv overlay: clang 23.1.0-wasi-sdk with `share/wasi-sysroot` for
`wasm32-wasip1`. Portage's own `llvm-core/clang` and `llvm-core/lld` (22.1.8)
have a complete WebAssembly backend, but the tree carries no wasi-libc, so the
host clang cannot link a WASI program. Guests use the SDK's compiler, core uses
the host compiler, and the two are not mixed. `wasi-sdk.pc` exports `clang`,
`clangxx`, `sysroot` and `target` as pkg-config variables, so the compiler path
is `pkg-config --variable=clang wasi-sdk` and not a hardcoded `/opt` path.

Wasmtime is `/opt/wasmtime-50.0.1`, installed by `dev-util/wasmtime-bin` from the
same overlay: headers in `include/`, `libwasmtime.so` and `libwasmtime.a` in
`lib/`, CLI at `bin/wasmtime`. Its `wasmtime.pc` resolves the whole integration
with `pkg-config --libs wasmtime`, and carries `-Wl,-rpath,/opt/wasmtime-50.0.1/lib`,
so a linked binary needs no ld.so.conf entry.

Both ebuilds live in the overlay at `/var/db/repos/local/dev-util/`. They install
their `.pc` into `$(get_libdir)/pkgconfig`, which is `/usr/lib64/pkgconfig` here
and on pkg-config's default search path; `/usr/lib/pkgconfig` is not searched and
was the original mistake. Editing an ebuild means regenerating that package's
`Manifest` too, or portage rejects it and silently falls back to a stale binpkg.

Wasmtime keeps a compilation cache in `$XDG_CACHE_HOME/wasmtime` by default,
which a sandboxed session cannot create and a `Read-only file system` error kills
the CLI outright. The daemon must configure the cache to its own `<state>/cache`,
not inherit the environment.

Dependencies resolve through pkg-config: Wasmtime C API,
ngtcp2, OpenSSL, tree-sitter, libcurl, sqlite3, libcbor, yyjson, zstd. Guests:
`clang --target=wasm32-wasip1` against wasi-sdk for C and C++, Zig for Zig (pin
the version — the language still breaks between releases), a thin C module
embedding Janet for policy.

`make test` runs the suite, one binary at a time, each under `timeout`.
Sanitizer builds are variables, not edits: `make BUILDDIR=build-asan
SANITIZE=address`, likewise `undefined` and `thread`, then `make test` with the
same `BUILDDIR` in each. `gate_m0` is the bench — 2,000 round-trips, and
it asserts 1,000/s unless it was built with ASan.

One build at a time, at the top level. A subagent edits and reasons; the
coordinating session runs the build and the suite and feeds the output back.
Never build release or `clean` on a guess.

## Layout

`src/core/` loop, sessions, capabilities, scheduler · `src/proto/` frames, CBOR,
method table · `src/wasm/` host, limits, instance pool, migration, ABI ·
`src/daemon/` socket, auth, subscriptions · `src/cli/` command line, headless ·
`modules/<name>/` first-party guests · `clients/` test client, then `dsh-native`
· `tests/` unit, golden transcripts, fuzz corpus · `docs/`.

Every guest has a manifest with `capabilities`, and the host refuses to
instantiate one whose capabilities are not granted. Adding a capability is a
core change and therefore needs the authority argument above.

## libcbor is not safe to hand untrusted bytes

`cbor_load` allocates from the lengths declared in the input, so a 41-byte frame
claiming a 2^39-element array hangs the decoder with no syscall in sight. Every
decode in core goes through `wd_decode`, which pre-scans with `scan_cbor`
(bounds, depth, chunk types) before libcbor sees anything. Do not call
`cbor_load` directly.

Its reference counting is also unusual: `cbor_map_add` and `cbor_array_push`
**incref** what they take, so the caller must `cbor_decref` its own reference
afterwards (which is what `wd_put` does for you). `wd_encode` **consumes** the item
it is given — it frees it, including on failure — so encoding a borrowed subtree
means a local `cbor_serialized_size`/`cbor_serialize` pair, never `wd_encode`. Getting
the reference counting backwards leaks every key and value; ASan only catches it because
the suite runs with leak detection.

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
- **Web search is a skill, not a tool.** It is judgement — what to search for, which result
  to trust — so it lives in the `skills` module, and core offers exactly one capability
  behind it: `web.search`, which runs the local `srchdsh` binary as a child. No module holds
  a search API key or a socket to one, and the engine list belongs to srchdsh's config, not
  to this repository.
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
- **MPL-2.0.** `LICENSE` is the verbatim Mozilla text, fetched rather than
  retyped; never edit it by hand, re-fetch it if it ever needs refreshing. Every
  new source file starts with the three-line Exhibit A notice from the bottom of
  that file. Not MIT, and not the IUL — this repository is open source.
- Every exported core symbol is `wd_`-prefixed, with the owner in the second segment
  (`wd_turn_*`, `wd_http_*`, `wd_session_*`). `static` helpers carry no prefix. There is one
  namespace, not one per module.
- Say when something is a guess rather than a measurement. The parity table is
  the scope contract; anything not in it waits.
