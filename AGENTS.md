# Working in this repo

A coding agent harness: a small C core that owns I/O and enforcement, and a
reloadable WebAssembly layer that owns every decision. Binary protocol over a
Unix socket; no web UI.

**Status: M1.** The daemon, the `AF_UNIX` transport, the handshake, sessions,
durable events, replay and a client exist and pass their gate, and the M1 layer sits on
top of them: the Wasmtime host, the provider, the engine/driver dispatcher and QUIC with
device pairing. `docs/protocol.md` is a frozen contract: the frame layout, the method names
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

Done, in the README's own order: `Makefile` and pkg-config deps with libcbor and zstd;
`docs/protocol.md`, frozen before the codec; `src/proto` frame and message codecs
with a fuzz gate; `ward up` with handshake, `session.create/list/get/resume`
and replay; a CLI client; and the M0 gate, which measures round-trips rather
than assuming them.

M1 is under way, with two pieces done and gated: the SSE framer (`src/proto/sse.c`),
bounded and fuzzed, which is the transport half of the provider; and the Wasmtime
host (`src/wasm/host.c`) with the module ABI frozen in `docs/modules.md` — engine,
store limits, fuel, the single `ward.cap` import, pure-module import enforcement and
state export/import, proven by `wasm-test` against real guests.

Third: the `engine`/`driver` pair as real guests and `src/core/turn.c`, the generic
dispatcher. The driver returns actions; core routes an `invoke` to the named module and
feeds the result back, and turns `emit` into an event. `driver` reaches `engine` only
through `ward.cap("engine.call")`, core-mediated. `turn-test` runs two prompts end to
end through all three guests.

Fourth: `session.prompt` is served and streams `text.delta`/`turn.end` as durable events,
and `ward session prompt <id> <text>` prints a headless turn.

Fifth: the provider is real. `src/net/http.c` posts to a loopback endpoint with a timeout
and a 4 MiB body cap, core frames the reply for the guest as `status + body`, and the
provider turns SSE `content` chunks into text. `gate-m1-test` runs a prompt against a
fake OpenAI endpoint that echoes the request, so request building and response parsing
are both proven. The HTTP call is synchronous inside the daemon's loop, which stalls
other sessions for the length of a completion; that is M1's known limitation and the
worker/async split is what removes it.

Sixth: QUIC and device pairing. `src/net/quic.c` speaks ngtcp2 over OpenSSL 3.5+ TLS: one
bidirectional stream per connection carrying frames, ALPN `ward`, a self-signed server
certificate pinned at pairing. `src/net/pki.c` owns the local CA in `<state>/pki/`, the
CRL, and the client certificates it issues, whose scope (`readonly`, `phone`, `desktop`) is
the organisational unit and is enforced by the daemon, not the client. `device.pair` mints
a five-minute single-use enrolment token; `device.enrol` is the only method an
unauthenticated QUIC connection may call and answers with a signed certificate;
`device.list` and `device.revoke` are local only, and a revoke also closes the device's
live connections. `gate-quic-test` (`quic-gate`) drives the real daemon: pair over the
socket, enrol over QUIC, create a session over QUIC with the issued certificate, prove a
`readonly` device is refused `session.prompt`, and prove a revoked certificate no longer
gets a stream.

Two traps cost an afternoon here. The daemon must service QUIC on epoll *timeouts* as well
as on socket readiness (`wd_quic_service` from the `n == 0` branch), or a
flow-control-blocked write is never retried and the peer waits forever — it presented as a
one-in-five hang. And the ALPN constant carries its length byte: `"\4ward"` with
`WD_QUIC_ALPN_LEN 5`, not the bare name.

Seventh: tools and approvals. `modules/tools` is one guest with one shape, `{tool, args}`,
which forwards to a capability and returns its reply; core implements `fs.read` and
`fs.write` against the turn's tool root, refusing `..`, absolute paths and symlinks, and
capping a read at 64 KiB. The policy guest decides which calls need approval and core
parks the action keyed `(session, call, epoch)`, emits `approval.requested` as a
durable event and answers `session.prompt` with `{session, approval: {call, epoch, reason}}`.
`approval.respond` records the answer, emits `approval.resolved`, resumes the turn and the
driver finishes with `turn.end`. A client that attaches later sees the request on replay; a
second answer is `already_resolved`, an unknown one `not_found`. `tool-test` drives the turn
directly and `gate-tool-test` drives the daemon, late attach included.

`edit` and `grep` complete the M1 tool set: `fs.edit` replaces the first occurrence of a
string in a file under the tool root, and `grep.run` shells out to `rg` from the root with
no shell, capped at 200 matches and 64 KiB. The policy allows `grep` (it only reads) and
asks for `edit`, exactly as it does for `write` and `exec`.

`exec` is the third tool: `exec.run` in core spawns one short-lived child in the turn's
tool root with a scrubbed environment, no shell, a ten-second default timeout and a 64 KiB
output cap, and the policy module makes it `ask`, so an exec-shaped call parks exactly like
a write. The per-session worker that will own long-lived children and the sandbox is still
to come; until then `exec` is a child of the daemon.

Policy is its own guest again: `modules/policy` is pure, with an empty import section like
`engine`, and answers `{tool}` with `{decision}` of `allow`, `ask` or `deny`. The driver asks
it through `policy.check`, a core capability that dispatches into the module, and core owns
only the gate that turns `ask` into a parked approval. `read` is allowed; `write` and
`exec` ask; anything else the module is not told about — an unknown tool — is denied, which
is the fail-closed default. The README's judge and plan gate are M2. A method is served only when
`docs/protocol.md` says it is served — the rest of the frozen list answers `not_supported`.

M0's acceptance is 1,000 round-trips/s over UDS and a fuzz-clean decoder. A
gate that is not measured is not met.

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
