# Working in this repo

A coding agent harness: a small C core that owns I/O and enforcement, and a
reloadable WebAssembly layer that owns every decision. Binary protocol over a
Unix socket; no web UI.

**Status: M0.** The daemon, the `AF_UNIX` transport, the handshake, sessions,
durable events, replay and a client exist and pass their gate. M1 is the Wasmtime
host. `docs/protocol.md` is a frozen contract: the frame layout, the method names
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

Done, in the README's own order: `meson.build` and options with libcbor and zstd;
`docs/protocol.md`, frozen before the codec; `src/proto` frame and message codecs
with a fuzz gate; `harness up` with handshake, `session.create/list/get/resume`
and replay; a CLI client; and the M0 gate, which measures round-trips rather
than assuming them.

Next: the SSE client, fuzzed before the provider client exists, then the Wasmtime
host and the `engine`/`driver` pair (M1). A method is served only when
`docs/protocol.md` says it is served — the rest of the frozen list answers
`not_supported`.

M0's acceptance is 1,000 round-trips/s over UDS and a fuzz-clean decoder. A
gate that is not measured is not met.

## Build

C11 and Meson, debug by default. `meson setup build -Dbuildtype=debug
-Dguest_profile=debug && meson compile -C build`. Two profiles, both Meson
options rather than edited build files: debug compiles guests at `-O0 -g` and
keeps DWARF; release is `-O2` and strips.

The guest toolchain is pinned at `/opt/wasi-sdk-34`: clang 23.1.0-wasi-sdk with
`share/wasi-sysroot` for `wasm32-wasip1`. Portage does not provide it —
`llvm-core/clang` and `llvm-core/lld` are installed (22.1.8) and their WebAssembly
backend is complete, but there is no wasi-libc in the tree, so the host clang
cannot link a WASI program. Guests use the SDK's compiler, core uses the host
compiler, and the two are not mixed.

Wasmtime is pinned at `/opt/wasmtime-50.0.1`: headers in `include/`,
`libwasmtime.so` and `libwasmtime.a` in `lib/`, CLI at `bin/wasmtime`. It ships
no pkg-config file and no CMake package, so meson has to use
`cc.find_library('wasmtime', dirs: ...)` plus `include_directories`, not
`dependency('wasmtime')`.

It keeps a compilation cache in `$XDG_CACHE_HOME/wasmtime` by default, which a
sandboxed session cannot create and a `Read-only file system` error kills the
CLI outright. The daemon must configure the cache to its own `<state>/cache`, not
inherit the environment.

Dependencies resolve through `dependency()` and pkg-config: Wasmtime C API,
ngtcp2, OpenSSL, tree-sitter, libcurl, sqlite3, libcbor, yyjson, zstd. Guests:
`clang --target=wasm32-wasip1` against wasi-sdk for C and C++, Zig for Zig (pin
the version — the language still breaks between releases), a thin C module
embedding Janet for policy.

`meson test -C build` runs the suite. Sanitizer builds are the options, not edits:
`meson setup build-asan -Dsanitize=address`, likewise `undefined` and `thread`,
then `meson test` in each. `gate-m0-test` is the bench — 2,000 round-trips, and
it asserts 1,000/s unless it was built with ASan.

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

## libcbor is not safe to hand untrusted bytes

`cbor_load` allocates from the lengths declared in the input, so a 41-byte frame
claiming a 2^39-element array hangs the decoder with no syscall in sight. Every
decode in core goes through `hx_decode`, which pre-scans with `scan_cbor`
(bounds, depth, chunk types) before libcbor sees anything. Do not call
`cbor_load` directly.

Its reference counting is also unusual: `cbor_map_add` and `cbor_array_push`
**incref** what they take, so the caller must `cbor_decref` its own reference
afterwards. Getting that backwards leaks every key and value; ASan only catches
it because the suite runs with leak detection.

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
- **MPL-2.0.** `LICENSE` is the verbatim Mozilla text, fetched rather than
  retyped; never edit it by hand, re-fetch it if it ever needs refreshing. Every
  new source file starts with the three-line Exhibit A notice from the bottom of
  that file. Not MIT, and not the IUL — this repository is open source.
- Say when something is a guess rather than a measurement. The parity table is
  the scope contract; anything not in it waits.
