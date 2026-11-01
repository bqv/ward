# ward

A coding agent harness with a small native core and a reloadable WebAssembly logic layer.
Feature parity with DSH, minus the web UI, with a binary protocol instead of HTTP.

Status: M0 implemented — daemon, `AF_UNIX` transport, handshake, `session.create/list/get/resume`,
durable events and replay, and a test client. The wire contract is frozen in `docs/protocol.md`;
M1 (the Wasmtime host) is next.

## Thesis

A small C core owns I/O, file descriptors and enforcement. Everything that *decides* is a
reloadable WASM module. Clients speak a binary protocol over a Unix socket.

The split follows Emacs: the core exposes primitives, not policy, and its ABI changes rarely,
while the logic above it changes constantly and reloads without dropping a session.

## Processes

```
ward (core)          one process, C, epoll
 └─ session worker      one sandboxed process per active session, for exec/PTY
     └─ commands        ordinary children of the worker
 clients                n, over a Unix socket
```

The sandbox is constructed once per worker, not once per tool call. No `bwrap` in the hot path.

### Session lifecycle

| state | loop instance | worker | when |
|---|---|---|---|
| hot | resident in core | alive | mid-turn, or PTY attached, or jobs running |
| warm | resident | reaped | idle, no terminal, no children |
| cold | exported, dropped | reaped | idle beyond a threshold |

- A worker is created on the first `exec` or `pty.open` and reaped a configurable time after
  its last child exits.
- An attached PTY pins the worker: the shell and its file descriptors cannot migrate.
- Running jobs, schedules and subagents pin the worker even when the user is idle.
- The loop instance can be exported to storage and dropped; waking instantiates the cached
  module and imports state in single-digit milliseconds.

Worker death is contained: the next tool call spawns a new worker and the tool returns an
error. Core death takes workers with it via `PDEATHSIG`; their namespaces go too.

Session state is one machine — `idle | running | awaiting_approval | interrupted` — and clients
render what they are told rather than inferring it, so the UI cannot disagree with the loop.

## The core (C11)

Responsibilities:

- epoll loop, timers, signals, `pidfd`, `signalfd`
- protocol server: framing, auth, subscriptions, replay, credits
- capability broker and audit log
- Wasmtime host: registry, versions, refcounts, drain and migrate
- scheduler: jobs, schedules, background work
- TOML config, SQLite for mutable state, zstd event logs per session
- CBOR and JSON codecs
- tokenizer

The tokenizer is native because BPE is a hot byte loop; a guest would pay for the copy twice.

### The ABI

Guests get primitives, never syscalls. Deny by default.

```
clock.now()                      -> {unix_ms, text}
random.bytes(n)
log.{debug,info,warn,error}
store.{get,put,del,scan}
fs.{stat,read,write,list,grep}   served by the worker's file broker (openat2, dirfd)
net.request(streaming)           -> handle; allowlisted hosts only
exec.run(argv, limits)            -> {handle, stream}; policy + approval before fork
exec.kill(handle)
tool.invoke(name, args)          -> result; the only way a module reaches a tool
engine.call(event)               -> {state, [action]}; core-mediated, granted only to `driver`
pty.{open,write,resize,read}
tokens.count(model, text)
judge.classify(kind, payload)    -> {verdict, reason, model, latency_ms}
web.search(query, k)             -> [{title, url, snippet}]; srchdsh, no socket in the guest
recall.search(query, k, scope)   -> [{text, source, score}]
code.outline(path)               -> [{name, kind, line, end_line}]
code.defs(name)                  -> [{path, line, kind}]
code.refs(name)                  -> [{path, line, kind}]
code.search(query, k)            -> [{text, path, score}]
```

Every call is logged with its decision. `--trace-abi` prints every crossing with arguments,
decision and result; it finds boundary bugs faster than stepping.

Keep calls coarse. `net.request` returns a streaming handle rather than a byte, and
`store.put` takes a batch. Chatty interfaces are how WASM cores get slow.

### Keeping it thin

The test for anything new: does it own authority or make a decision? Authority — a file
descriptor, a socket, a child process, a durable invariant, a capability check — belongs in
core. A decision — what to say, what to run, how to rank, when to fire — belongs in a module.

Core is therefore allowed: the event loop, the protocol server, the capability broker, the
Wasmtime host and registry, the store, PTY and process spawning, config, and the codecs.
Everything else has to justify itself. No prompt text, no tool logic, no provider quirks, no
ranking, no policy rules, no cron arithmetic in core.

Three tenants are on notice. The tokenizer is native for speed; if it grows a vocabulary it
becomes a `tokens` module with a cache. Judge and recall do their own HTTP over scoped `net`
grants rather than calling a core client. Scheduling decisions are computed by a module; core
owns only the timer wheel. Budget: core stays under roughly eight thousand lines, and an
addition that pushes past it must delete something or be shown to be authority.

### Module ABI

Modules are core wasm modules targeting `wasm32-wasip1`, not components, at least in v1.
The component model and WIT still churn; the C ABI is stable and language-agnostic.

```
ward_alloc(len) -> ptr
ward_call(ptr, len) -> {ptr, len}                  // one message, CBOR in and out
ward_state_export() -> {ptr, len}                  // serialised state, CBOR
ward_state_import(ptr, len)                        // may fail
ward_state_migrate(from_schema, ptr, len) -> {ptr, len}
```

Each module ships a manifest:

```toml
name = "grep"
version = "2"
state_schema = 1
capabilities = ["fs.read", "fs.list", "fs.grep"]
exports = ["call", "state_export", "state_import"]
lazy = false
```

The host refuses to instantiate a module whose capabilities are not granted. `lazy = true`
defers instantiation to the first call within a turn: `python` sets it, so a turn that never
asks for Python pays neither the module load nor the interpreter start-up.

## Modules

What runs in WASM, and therefore reloads without dropping a session:

| module | owns | language |
|---|---|---|
| `engine` | pure transition function, import-free | C++20 |
| `driver` | action interpreter, prompt assembly, time injection, compaction, plan gate | C++20 |
| `provider` | request building, SSE parsing, reasoning blocks | C++20 |
| `tools` | read, write, edit, grep, exec wrappers, ask | Zig |
| `python` | embedded CPython kernel and ward bindings, lazy and per turn | C |
| `recall` | chunking, ranking and snippet shaping over the core-held index | Zig |
| `code` | tree-sitter grammars, outline and symbol queries | Zig |
| `policy` | which calls require approval, including judge escalation | Janet |
| `skills` | AGENTS.md, skill loading, slash commands, web search | Zig |
| plugins | event hooks | Zig, C or C++ |

### Web search is a skill

Searching the web is neither a core tool nor a provider feature: it is a skill, because what
to search for, which result to trust and when to stop are judgement rather than I/O. The
`skills` module owns that procedure and calls one core capability, `web.search(query, k)`,
which runs the local metasearch binary — `srchdsh search -q … -json` — as a child of core.
The module never holds a socket or an API key, and adding, removing or reweighting engines
is a change to srchdsh's configuration, not to ward. Results are capped, timeout-bounded and
logged with their decision like every other crossing.

Languages are chosen per module, not per project: C for boundaries, C++ for the two modules
that share a data model, Zig for I/O and text, Janet where a decision should be editable
without a compiler. `engine`, `driver` and `provider` stay in one language because they exchange the
message types; everything else meets at CBOR and need not.

### The engine is its own module

`engine` is a separate guest with an **empty import section**, enforced at load. It is the pure
half: `engine.call(event) -> {state, [action]}`, no capabilities, no host imports, POD state,
one entry point. That is the translation unit CBMC checks.

`driver` is the loop's effectful half: prompt assembly, time injection, compaction, plan gate,
and the action semantics. It is *also* a state machine, and it suspends at every tool call by
returning:

```
driver.step(event) -> {state, [action]}
```

A suspension is a return, not a blocking host call: wasm cannot pause an instance mid-import,
so the driver hands the action back and is re-entered with `{tool_result, ...}` when the tool
finishes. The action carries the call id the driver minted, which approvals and the journal key
on.

Core owns no action semantics — only a generic dispatcher that routes
`{invoke, module, name, args}` to the named module, awaits, and feeds the result back. No tool
logic, no prompt logic and no compaction logic lives in core.

The driver's capabilities are `clock.now`, `tokens.count`, `recall.search` and `engine.call`.
Nothing else: it cannot read a file, run a command or reach the network. The tool it invoked
runs the effect under its own manifest.

`engine.call` is the only module-to-module call, core-mediated and granted only to `driver`.
Every other name in the ABI (`judge.classify`, `recall.search`, `code.*`) is a core
capability: the caller sees one call, and core may dispatch it into the module that implements
it — recall embeds over its scoped `net`. None of them is a module-to-module call.

### Provider scope

`provider` gets `net` scoped to the endpoints in `models.toml`. Core derives the allowlist from
the same data the model catalog uses, so adding a provider is a data edit rather than a
manifest edit, and any host not in the catalog is refused; a new host fires a confirmation
event before use. One module per backend would give a stronger boundary at the cost of a module
per vendor — the right move later, if the catalog ever becomes multi-tenant. Credentials belong to the provider: core passes
the key into its config and keeps nothing. Core hosts the guest and can read its memory, so the
boundary that matters is that the key never reaches the worker or an `exec` child.

Policy runs in Janet: one C99 library, no host language, `policy.janet` re-evaluated when the
file changes. It is pure — it receives the call and returns `allow | ask | deny` with a reason —
so there is no state to migrate, and the core keeps a non-bypassable floor underneath: a
permissive policy cannot widen the sandbox.

### Judge-assisted policy

Rules handle what they can; anything unclassified can be escalated to a local model through
`judge.classify`. The judge module performs the call over a `net` grant scoped to llamb on the
GT 1030 (`127.0.0.1:55556`); the core owns only the contract — monotonic merge, cache, timeout
and audit — so the prompt and the endpoint policy stay in the guest.

- **Monotonic.** The verdict is `max(rule, judge)`. The judge can raise `allow` to `ask`, never
  lower a `deny` or an `ask`.
- **Fail closed.** Timeout, connection refused, malformed output, or no model loaded: fall back
  to the rule verdict. If that verdict is `ask` and no client can answer, it becomes `deny`.
- **Cached.** Keyed on `(policy version, tool, raw argument bytes)` and persisted, so repeated
  calls do not each cost a model round trip and a benign entry cannot be warmed into serving a
  hostile variant.
- **Recorded.** Every decision emits `judge.decision` with model, prompt hash, latency and
  verdict, so a replay can explain why a command was allowed.
- **Delimited.** The payload is attacker-controlled text. It travels as data inside a fixed
  template and the answer is parsed as a single token.

The judge is an escalation, not a dependency: with llamb down the rule verdict stands, and an
`ask` with no client to answer it becomes `deny` — exactly what happened before the feature
existed.

`python` is the eval surface: a C guest embedding CPython built for `wasm32-wasip1`, plus a
`ward` extension module exposing `read`, `write`, `grep`, `exec`, `fetch`, `state` and
`log`. Thin pure-Python shims map common stdlib calls onto those bindings — `open`,
`subprocess.run`, `urllib.request`, `os.getenv` — with each divergence documented, so the
model's usual idioms work without pretending the missing stdlib exists. Each of those is a host import, so approvals and audit apply inside the script
rather than around it. The interpreter is **lazy and per turn**: nothing is instantiated
until a turn actually calls it, and the instance is destroyed at the boundary. A live
CPython heap cannot be exported, so anything that must outlive the turn goes through `ward.state`, which is the core's store and is
therefore migratable. Per-turn keeps sessions replayable and gives the module the same
versioning and reload behaviour as every other guest.

`recall` is the ward's memory, split the same way as judge. The core owns the index: vectors in a cache beside
the session logs, and the incremental indexing job (session closed, `~/ref`
file changed). The module does the text work — chunking, query shaping, ranking, snippets —
and calls the embedder over a `net` grant scoped to llamb. That scoped `net` is its only
capability; it never touches fs or the store directly. `recall.search(query, k, scope)` takes
`sessions`, `notes` or `all`. During migration the old `dsh-recall` service is a source for
DSH history; once the ward's own sessions are indexed it is no longer a dependency.

`code` is tree-sitter, and deliberately not an IDE. The core owns a symbol index in a cache beside the
session logs: `symbols(path, name, kind, line)` and `edges(from, to, kind)`,
built by a background job that reparses a file when its hash changes. The module holds the
grammars and does the parsing and querying, so adding a language is a module update rather
than a core one.

Liveness is handled by not pretending to be live. `code.outline` parses the file on demand and
is therefore always current. `code.defs` and `code.refs` answer from the index and carry its
generation, so a caller can tell when it is stale; a changed file is reparsed on the next
index pass, not mid-answer. `code.search` reuses the recall vectors under a `code` scope.
There is no LSP anywhere: no server lifecycle, no `compile_commands.json`, no per-language
daemon.

Time injection lives in `driver`: every turn's prompt is assembled with `clock.now()` — local
date, time, timezone — refreshed per turn, and frozen in tests by stubbing the capability.
Per-message timestamps are recorded in the transcript for replay.

Do not reimplement syscall-heavy tools in WASM. Grep runs in the worker's file broker,
backed by `rg`; the module decides paths, patterns and result shaping. Hot loops stay native, decisions stay reloadable.

## State and migration

A session's state is a CBOR record: messages, queue, goal, compaction summary, tool state,
model, permissions, timestamps.

Upgrade order: drain to a message boundary, `state_export`, drop the old instance, instantiate
the new version, `state_import`, running `state_migrate` between schema versions. If import
fails, keep the old instance and report; the session survives.

Old instances stay alive until their refcount reaches zero. This gives a hot swap at the next message boundary when a session is upgraded, and declared
migration of a record, not a stack. A turn is a message boundary, so it
covers the case that matters.

The `python` module is the exception that proves the rule: a CPython heap cannot be exported,
so its interpreter lives exactly one turn and has nothing to migrate. Persistent data belongs
in `store`, reached from Python as `ward.state`, which the core versions like any other
session record.

## Storage

Three tiers, and only the first is a database.

**`config/*.toml`** — human-authored and diffable: the model list, defaults, judge endpoint,
paths, policy location. Watched, validated, and reloaded on change.

**SQLite (`ward.db`, WAL)** — app-mutable state that benefits from atomic row updates and
that you may still edit by hand: `settings` overrides, session labels (`title`, `pinned`,
`archived`, `order`), the device registry, and module pins. It is
validated on load, reloaded when `PRAGMA data_version` changes, and every accepted change emits
`config.changed`. Direct `sqlite3` edits work because that reload path exists.

**Session logs (`sessions/<id>/events.zst`)** — append-only and authoritative for content:
framed CBOR records, zstd-compressed, with an immutable header carrying id, cwd, created and
the model the session started on. Never rewritten, and no transcript is duplicated into SQLite.

**Caches (`cache/`)** — recall vectors, code symbols, anything else derived. Flat binary
sidecars, disposable, rebuilt by scanning the logs when missing or stale. Losing one costs a
rebuild and nothing else. Logs grow forever in principle; retention is a policy setting
(`sessions.keep_days`) that archives or deletes whole sessions and never rewrites one.

Full-text search is not a table: recall covers semantic retrieval, `rg` covers exact matching
by decompressing a log on demand, and if that ever gets slow the answer is a plain-text sidecar
cache rather than an FTS index in the database.

## Recompose

Three tiers, with different costs.

| tier | what | takes effect |
|---|---|---|
| data | model list, prices, limits | immediately, next prompt |
| bindings | tool set, provider, policy, prompt fragments | next turn |
| code | loop, tools, skills modules | hot swap at a boundary, with migration |

The model list is data, not code: rows in `models.toml` (`id, provider, model, context, price,
tags`), watched and reloaded on change, or written through the `model.*` RPC which rewrites the
file atomically. SQLite is not involved. A parse failure keeps the last good list in memory and
reports it; the daemon never serves a half-parsed catalog.

```
ward model add deepseek:deepseek-v4-flash --ctx 128k --price 0.04,2,8
ward tool enable grep@2 --session <id>
ward module reload loop@3
```

Composition is read at turn start and held for the turn. That is atomic by design: no tool
disappears under a running call, and the transcript always reflects what actually ran.

With `config.watch = true`, changes to `models.toml` or `policy.toml` emit
`config.changed`; idle sessions adopt immediately, busy sessions at their next boundary.

## Protocol

The frame is transport-independent: `u32 len, u16 type, u64 sub, u64 seq, CBOR body`.

- **Local clients** — `AF_UNIX`, SOCK_STREAM. No packet boundaries needed once the length is
  explicit; SEQPACKET's only advantage was boundaries for free, and it costs portability.
- **Remote clients** — QUIC over ngtcp2 + OpenSSL, one bidirectional stream for requests and
  one per subscription. Streams mean a 40 KiB tool result cannot block a token delta.

Auth is two layers:

- **Transport.** A self-signed server certificate whose fingerprint is pinned at pairing.
  Devices authenticate with client certificates issued by a small CA in the config directory.
  `ward device pair` mints a five-minute enrolment token and prints a QR carrying the token,
  the fingerprint and the endpoint; `ward device revoke` writes the device to a CRL
  consulted at handshake. Certificates carry a scope: `readonly`, `phone`, `desktop`.
- **Session.** A token minted at `session.create`, bound to `(device, session, epoch)`, required
  on `prompt` and `approval.respond`. Approvals are single-use and expire.

No frame is accepted on 0-RTT resumption: the whole session is 1-RTT. It costs a round trip on
reconnect and removes a replay surface entirely.

```
handshake {v, client, caps, cert, token?}
request   {id, method, params}
response  {id, ok|err, result}
event     {sub, seq, kind, payload}
control   {cancel:id} | {credit:sub,n} | {replay:sub,from} | {resync:sub}
```

- Every event carries `(sub, seq)`. Deltas are coalesced; a reconnecting client calls
  `session.resync` and gets a snapshot with the sequence it is valid at. Replay serves durable
  events only, so a gap in deltas is expected and recoverable by design — which is why M2's
  acceptance is *no lost durable events*, not no lost frames.
- Requests are idempotent per `(device, epoch, id)`: a retried `prompt` returns the original
  turn rather than starting a second one, which is what makes a timeout safe to retry.
- Credit windows per subscription stop a slow client ballooning the daemon. When a client is
  under credit, deltas are dropped and coalesced — never blocked, because blocking would stall
  the turn.
- Messages over 64 KiB are chunked with `{more, off}`; paginated methods declare a page size.

Methods:

```
session.{create,list,get,resume,fork,rename,archive,pin,search,prompt,cancel,compact,
         checkpoint,undo,queue,set_model,set_tools,set_policy,resync}
model.{list,select}
tool.{list}
approval.{respond}
subagent.{prompt,interrupt}
job.{list,kill}
schedule.{list,set,delete}
goal.{set,clear}
settings.{get,set}
device.{pair,enrol,list,revoke}
terminal.{open,write,resize,close}
file.{upload,read}
module.{list,reload}
stats.{session,global}
```

Events:

```
turn.start  reasoning.delta  text.delta
tool.call   tool.result      approval.requested  approval.resolved
session.updated  context.compacted  job.updated  subagent.updated
terminal.output  judge.decision  error
```

Approvals are **core state**, not module state. Core parks the action keyed by
`(session, call-id, epoch)` and broadcasts `approval.requested` to every client attached to the
session, including one that attaches later. First valid `approval.respond` wins; later ones get
`already_resolved`. The request carries the reason string and the asking agent or subagent
label. Because the approval lives in core, a module reload or a dropped client cannot lose it,
and the worker does not fork until a resolution is recorded.
## Feature parity with DSH, minus the web UI

| DSH feature | lives in | phase |
|---|---|---|
| agent loop, tool dispatch | modules `engine`/`driver` | M1 |
| providers, streaming, reasoning blocks | module `provider` | M1 |
| read/write/edit/exec/grep | modules + worker | M1 |
| permissions, plan gate, approvals | core gate + module `policy` | M1 |
| time context injection | module `driver` | M1 |
| model catalog, switching | core data + protocol | M1 |
| headless `-p` | CLI client | M1 |
| sessions: list/resume/fork/rename/archive/pin/search | labels in SQLite, content in logs | M2 |
| compaction, queue, checkpoints, undo | module `driver` + core store | M2 |
| token and cost accounting | core tokenizer + module | M2 |
| subagents | module `driver` + core supervisor | M3 |
| jobs, background notices | core scheduler | M3 |
| schedules (cron) | core scheduler | M3 |
| goals | module `engine` | M3 |
| skills, AGENTS.md, slash commands | module `skills` | M4 |
| web search | module `skills` over core `web.search` (srchdsh) | M4 |
| MCP client | module | M4 |
| terminal/PTY | worker + protocol | M4 |
| attachments, uploads | core + protocol | M4 |
| module reload and migration | core | M5 |
| native client, then Android | clients | M6 |

## Deliberately out of scope

- web UI, nginx/cookie chain
- a JavaScript shim for DSH plugins
- ACP in v1; can be added later as another face over the same protocol
- Windows and macOS
- images until M4

## Milestones

| phase | deliverable | acceptance |
|---|---|---|
| M0 | daemon, UDS transport, handshake, `session.create/list`, `replay`, test client | 1k round-trips/s over UDS; fuzz-clean decoder |
| M1 | Wasmtime host, ABI, provider SSE, engine and driver, tools, approvals, QUIC and device pairing | headless turn completes against a local endpoint; a paired device connects over QUIC |
| M2 | SQLite sessions, resume, fork, cancel, compaction, cost | kill -9 mid-turn, resume, no lost durable events |
| M3 | subagents, jobs, schedules, goals, plan gate | two subagents run concurrently under one session |
| M4 | skills, AGENTS.md, MCP client, terminal, python eval, recall, judge | repo rules load; one MCP server works; a `python` call returns a result; `recall.search` returns a note |
| M5 | module versions, drain and migrate, reload, code index | upgrade a stateful module mid-session with no data loss; `code.defs` finds a symbol in the harness |
| M6 | `dsh-native` transport, then Android; parity audit | phone drives a real session over the socket |

## Limits, recovery and upgrades

Fences, in order of severity:

- **Cgroup per session** — `memory.max`, `pids.max`, `cpu.weight`, the same shape DSH uses.
  It bounds the workers, not the guests: guest memory is daemon RSS, which is why the next
  fence exists.
- **Wasmtime store limits** — per-instance memory maximum and an instance cap, so a bad guest
  traps itself rather than the session.
- **Deadlines** — fuel or epoch per call. Cgroups bound CPU share, not wall time, and a looping
  guest will burn its share forever.
- **Side-effect journaling** — before any action that can have an effect (`exec`, `fs.write`,
  `net.request`, an MCP call) the dispatcher writes an intent record keyed by call id; after
  the effect it writes the result. A restart that finds a pending
  intent with no result marks the turn `interrupted` and never retries the effect
  automatically: the client sees "may have happened" and the model decides. At-most-once with a
  visible unknown, never silent exactly-once theatre.
- **Per-subscription buffer caps** and a per-turn tool-output cap, so one `cat /dev/urandom`
  does not become a two-gigabyte event.
- **Trace ids** — one per turn, one per tool call, emitted at `trace` level and carried into the
  worker. A loop of 179 identical calls should be visible in ten seconds, not discovered by
  watching the database grow.

Crash and restart:

- The daemon writes `<state>/ward.lock` with `{pid, boot_id, started_at}`, `O_EXCL`
  and stale-detected by boot id. Two daemons can never share a state directory.
- Workers get `PDEATHSIG(SIGTERM)` and live in the session cgroup, so `cgroup.kill` reaps the
  tree. On daemon death a worker finishes the operation it is in, bounded by its own timeout,
  and exits.
- On restart, sessions resume from their logs. Any turn that was in flight is marked
  `interrupted` and the client is told; nothing resumes silently.

Module upgrades are three separate mechanisms:

1. **Pinning** — a session records the module versions it started with. Reloads apply to new
   sessions by default; an existing session moves only on `session.upgrade`, at a boundary.
2. **Migration failure** — if `state_import` fails, the old instance stays and the session
   reports `migration_failed`. Automatic and per session.
3. **Bad version** — repeated traps mark a version bad in the registry; new turns use the
   previous one until cleared. Administrative and global.

Provenance: the registry refuses modules whose manifest is unsigned or whose hash is not in the
allowlist, and installs only through `module.install`, never by scanning a directory.

Module artifacts are one file. The signature lives in a wasm custom section, `ward.sig`,
holding `{alg, key_id, sig}` over the SHA-256 of the module with that section removed; the
artifact is then zstd-compressed, so `grep@2.wasm.zst` is what you copy and what the registry
verifies after decompression. Custom sections are ignored by every wasm tool, so the bytes
still load anywhere else. A connected client may request a version,
never supply bytes.

Testing: every provider call goes through a recorded cassette layer keyed by request hash, so
the suite runs without a model and a replay can explain a decision. Module conformance is one
suite every guest language must pass: allocate, call, export, import, migrate, trap on purpose.

## Risks

| risk | mitigation |

| risk | mitigation |
|---|---|
| memory and lifetime bugs in C | ASan, UBSan, TSan, `clang --analyze` in CI from day one; arenas per session |
| SSE parsing across chunk boundaries | fuzz the parser before the provider client exists |
| chatty host ABI | coarse capabilities; measure crossings per turn |
| sandbox edge cases: `/proc`, `/dev`, uid maps, SELinux labels | start with Landlock + seccomp; add namespaces only when needed |
| scope creep | the parity table is the contract; anything not listed waits |
| tree-sitter grammars rot per language | one grammar at a time, gated on the token measurement |
| judge latency and prompt injection | monotonic verdicts, fenced input, cache, fail closed |
| CPython-on-WASI build is the long pole | lazy per-turn interpreter, trimmed stdlib, a timeboxed spike before M4; if it overruns, ship `exec python3` in the worker instead |
| guest debugging | build guests with `-g`, keep DWARF, use Wasmtime's native-debugger support; `--trace-abi` for boundary bugs |

## Build

C11 and GNU make. Dependencies: Wasmtime C API, ngtcp2, OpenSSL, tree-sitter, libcurl, sqlite3, libcbor, yyjson, zstd,
resolved through pkg-config.

Guests are built per language: C and C++ through `clang --target=wasm32-wasip1` against
`wasi-sdk`, Zig through `zig build-lib -target wasm32-wasip1` (pin the Zig version; the language
still breaks between releases), and the policy guest as a thin C module embedding Janet.
The `python` guest additionally needs a cross-built `libpython` for `wasm32-wasip1` and a
trimmed standard library zip.

```
make -j4
make test
```

Two profiles: a debug profile compiling guests at `-O0 -g` with DWARF retained, and a release
profile at `-O2`. Both are make variables, not edited build files.

## Repo layout

```
ward/
├── src/core/        loop, sessions, capabilities, scheduler
├── src/proto/       frames, CBOR, method table
├── src/wasm/        registry, instance pool, migration, ABI
├── src/daemon/      socket, auth, subscriptions
├── src/cli/         command line, headless
├── modules/         first-party wasm guests, one directory per module
├── clients/         test client, then dsh-native transport
├── tests/           unit, golden transcripts, fuzz corpus
└── docs/protocol.md
```

## First two weeks

1. `Makefile` and pkg-config deps, CI with the sanitizers.
2. `docs/protocol.md`: freeze the frame table and method list before writing code.
3. `src/proto`: frame codec, CBOR wrapper, fuzz harness.
4. `ward up` serving the socket with handshake and `session.create`, plus a CLI client
   printing events.
5. The SSE client, before anything touches a model.
