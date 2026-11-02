# ward protocol

Version 1, frozen at M0. This file is the wire contract: method names, event kinds, error
names and the frame layout are a compatibility surface. Anything that changes them is a
protocol version bump, not an edit.

The design rationale lives in `README.md` under **Protocol**; this file is the exact shape
that is served. Where they differ, this file is what the daemon actually does and
`README.md` gets fixed.

## Scope of M0

Implemented: `AF_UNIX` transport, handshake, `session.create/list/get/resume`, durable
events and replay, the error taxonomy, client request dedupe.

Not implemented, and refused with `not_supported` if asked: everything else in the frozen
method list below — tools, approvals, subagents, jobs, schedules, goals, settings,
terminal, file transfer, modules, stats. QUIC, device certificates, session tokens and
message chunking arrive with M1/M4; until then the frame body cap is 64 KiB and a larger
message is `limit`.

M1 adds one method: `session.prompt`, which runs a turn and streams its events. It is
described below with the session object.

Later milestones add methods without a version bump, because adding a method is compatible.
`model.probe {id}` answers `{id, n_ctx, n_slots, context_source}` — the window and slot count the
provider reports, or the catalog's `context`, or a default, and which of the three it was.
`module.list`, `module.reload`, `session.upgrade`, `stats.session`, `stats.global`,
`attachment.add`, `attachment.list`, `terminal.*`, `schedule.*`, `job.list`, `job.kill`,
`subagent.prompt`, `subagent.interrupt`, `recall.search`, `mcp.call` and the rest. The one
that is read-only and worth knowing about here is `prompt.preview {id}`: it returns
`{session, sections: [{name, order, source, text}]}` — the assembled system prompt for that
session, in the order it will be sent, with each section's origin. It runs no turn and spends
no tokens.

## Transport

Local clients use `AF_UNIX`, `SOCK_STREAM`. The socket is created mode `0600` and the
daemon accepts only a peer whose `SO_PEERCRED` uid equals its own effective uid; anything
else gets `unauthenticated` and is disconnected. There is no session token in M0 — a
local uid check is the whole transport story, and the device/QUIC layer is what adds
certificates and tokens.

Paths:

- socket: `$XDG_RUNTIME_DIR/ward.sock`, else `/tmp/ward-$UID.sock`
- state: `$XDG_STATE_HOME/ward`, else `~/.local/state/ward`
- lock: `<state>/ward.lock`, holding `{pid, boot_id, started_at}`

One state directory is served by one daemon. A second `ward up` on the same state
directory refuses to start.

Remote clients use QUIC: TLS 1.3 over UDP, one bidirectional stream carrying the same frames
in both directions, ALPN `ward`. There is no HTTP/3 layer and no 0-RTT. The default
bind is `127.0.0.1:0`, loopback on an ephemeral port, so a daemon never collides with
another; `WARD_QUIC_BIND` sets it (`host:port`, `off` to disable the listener), and
`ward up` prints `quic <endpoint> <fingerprint>` when it is listening. The endpoint a
device needs comes from `device.pair`, not from a hardcoded port.

The server certificate is signed by a local CA in `<state>/pki/`, created on first start;
its SHA-256 fingerprint is what pairing hands to the device. A device presents a client
certificate from that CA whose organisational unit carries its scope (`readonly`, `phone`,
`desktop`). A connection without a certificate is unauthenticated: it may send exactly one
request, `device.enrol`, and any other method answers `unauthenticated` and closes the
connection. The CRL beside the CA is consulted for every peer certificate, so a revoked
device is refused at handshake.

## Frame

Every message, in both directions, is one frame:

| offset | size | field | notes |
|---|---|---|---|
| 0 | 4 | `len` | body bytes, `u32` little-endian, `0 < len <= 65536` |
| 4 | 2 | `type` | `u16` little-endian |
| 6 | 8 | `sub` | `u64` little-endian; subscription id, 0 when unused |
| 14 | 8 | `seq` | `u64` little-endian; event sequence, 0 when unused |
| 22 | `len` | body | CBOR |

`type`: `1 hello`, `2 welcome`, `3 request`, `4 response`, `5 event`, `6 control`.
`0` and values above `6` are reserved and are a protocol violation.

A frame with `len > 65536` is a violation, not a chunk: chunking with `{more, off}` is
M4's job and does not exist yet.

## Bodies

CBOR, definitive lengths, text keys, no tags and no floating point. Unknown map keys are
ignored, so a newer peer may add fields. Every structure below is a map.

```
hello    {v: 1, client: "ward-cli/0.0.1", caps: ["events"]}
welcome  {v: 1, server: "ward/0.0.1",
          auth: {uid: <uint>, scope: "local"},
          limits: {body: 65536, inflight: 64}}
request  {id: <uint>, method: <text>, params: <map>}
response {id: <uint>, ok: true,  result: <any>}
         {id: <uint>, ok: false, error: {code: <uint>, name: <text>, message: <text>}}
event    {kind: <text>, payload: <map>}
control  {replay: <sub>, from: <seq>}
         {credit: <sub>, n: <uint>}
         {cancel: <request id>}
```

The first frame a client sends must be `hello`, and it must arrive within five seconds of
accept. The daemon answers `welcome` and then serves requests. A version mismatch is
`version`, followed by disconnection.

Request ids are `>= 1` and unique per connection. A connection may have at most 64
requests outstanding; the daemon is single-threaded in M0, so in practice it answers in
order.

## Methods

Each request carries a method name and a params map. A result is a map unless the method
says otherwise.

### Served in M0

**`session.create`** — params `{cwd?: text, model?: text}`. Creates a session directory
and its log, writes the `session.updated` event, and returns
`{session: <session>}`. `cwd` defaults to the daemon's working directory; `model` defaults
to `"unset"`. The id is minted by the daemon, never supplied by the client.

**`session.list`** — params `{limit?: uint}` (default 100, maximum 1000). Returns
`{sessions: [<session>, ...]}`, newest first by `created`.

**`session.get`** — params `{id: text}`. Returns `{session: <session>}`, or `not_found`.

**`stats.session`** — params `{id}`. Counts the session's log: `{session, model, events,
turns, text_bytes, prompt_bytes, tokens_estimated}`, where `events` is `last_seq`, `turns`
the number of `turn.end` events, `text_bytes` the total length of the text deltas and
`prompt_bytes` the length of what was asked. When the session's model carries a `price` in
`models.toml` the answer also has `{cost_in, cost_out, cost_total}` in the currency those
numbers were written in.

`tokens_estimated` is true because there is no tokenizer yet: the cost divides bytes by
four. It is labelled so a client can never mistake the number for a measured one, and the
fields stay the same when a real tokenizer replaces the estimate.

**`stats.global`** — no params. The same counters summed over every session, plus
`{sessions}`.

**Attachments.** `attachment.add {id, name, data}` writes `data` (bytes or text, capped at
4 MiB) to `<state>/<session>/attachments/<name>` — the name is refused if it holds `/` or `..`
— appends a durable `session.attachment {name, size}` event and answers `{path, size}`.
`attachment.list {id}` replays those events into `[{name, size}]`. The file lives in the state
directory, not the tool root, so a turn cannot read it without being told the path. Before a
prompt runs, core also sends the attachments as a `system` message — `attached to this
session: note.txt (13 bytes)` — so the model knows what is there.

**Recall.** `/recall <words>` asks the skills guest, which calls core's `recall.search
{query, k}`. Core runs the local `dsh-recall search <query> -k <k>` as a child (no shell, 20 s,
64 KiB) and answers `{ok, status, text}`. The index, its embedding endpoint and its ranking
belong to `dsh-recall`; ward only runs it.

**Python.** `/py <program>` runs `python3 -I -c <program>` in the session's cwd through the
worker's child runner — 15 s, 64 KiB — and answers `{ok, status, text}`. The policy asks for
approval, because the interpreter can read anything the worker can. This is the host
interpreter in the worker, not the embedded-CPython guest the design describes; the method and
its shape stay when that guest lands.

**Judge-assisted policy.** `policy.check {tool, payload}` asks the policy guest for a rule
decision; when that decision is `allow`, the `judge` guest may raise it to `ask` by classifying
the payload. The merge is monotonic — a judge can never lower an `ask` or a `deny` — and if the
judge fails or answers nonsense the rule stands. An escalated call parks exactly like a rule
`ask`, so the client sees an ordinary `approval.requested`. Whenever the judge is consulted the
turn appends a durable `judge.decision {tool, verdict, escalated}` event, so a replay can
explain why a call was allowed or paused.

**Terminals.** `terminal.open {id, command?, rows?, cols?}` starts a command under a
pseudo-terminal in the session's cwd (default `sh -i`) and answers `{terminal, pid}`.
`terminal.write {terminal, data}`, `terminal.resize {terminal, rows, cols}` and
`terminal.close {terminal}` drive it, answering `{written}`, `{resized}` and `{closed}`.
Output arrives as `terminal.output {terminal, data}` — a **non-durable** broadcast, not a logged
event: a terminal can produce unbounded text and the session log is not its place. The stream
ends with `terminal.output {terminal, eof: true}`; the handle is then invalid.

**MCP.** `/mcp <json request>` sends one JSON-RPC line to the configured MCP server: core's
`mcp.call` capability runs the server binary (`WARD_MCP_BIN`, else `~/bin/mcp`) as a child,
writes the request to its stdin, closes it, and answers `{ok, status, text}` with the
server's first response. One request per process is the M4 shape; a persistent session is
what a later milestone would add.

**Web search.** `/search <terms>` is handled by the `skills` guest, which calls core's
`web.search` capability with `{query, k}`. Core runs the local `srchdsh` binary
(`search -q … -max … -json`) as a child with no shell, a 20 s timeout and a 64 KiB cap, and
answers `{ok, status, text}`. The module decides what to search for and which result to trust;
core only runs the binary and returns its words.

**Project rules.** Before a prompt runs, core reads `<session cwd>/AGENTS.md` (up to 2 KiB) and
sends it to the provider as the first history entry, a `system` message. Slash commands skip
it; a turn whose root has no AGENTS.md sends none.

**Slash commands.** A prompt beginning `/` that no tool command claims is offered to the
`skills` guest, which answers `{handled, text}`. `/help`, `/rules` and `/skills` are handled
there today; anything unhandled falls through to the model as an ordinary prompt.

**The plan gate.** A prompt beginning `/plan ` asks the policy module for `plan`, which answers
`ask`, so the turn parks on an approval with that reason like any write. Allowing it emits the
approved text and finishes the turn; denying it ends with the policy's refusal. The plan is a
tool call in the ordinary shape — `plan.run` in core — so a client cannot tell an approved
plan from an approved command.

**`schedule.set`** — params `{session, cron, text}`. The `schedule` guest turns the expression
into the next fire time (cron fields, or `@every <ms>` for an interval), core stores the entry
and appends `schedule.updated {schedule, state, next_ms}`. Answers `{schedule, next_ms}`. When
it is due the daemon forks the session, runs `text` as a turn in the child and appends
`schedule.fired {schedule, session}` before re-arming. `schedule.list` answers
`{schedules: [...]}` and `schedule.delete` answers `{deleted}`.

**`goal.set`** — params `{session, text}`. Appends a durable `goal.set {text}` and answers
`{goal}`; `goal.clear` appends `goal.cleared`. The active goal is prepended to the history
every later prompt sends, as a `system` message, so the model is reminded what the session is
for without the client repeating it.

**`job.list`** — no params. Answers `{jobs: [{job, session, state}]}` for every turn
currently running or waiting; the job id is the session it runs in. `state` is `running` or
`queued`. At most `WARD_MAX_RUNS` turns (eight by default) run at once; a background turn
started past the cap is queued, answers `{session, queued: true}`, and begins when a running
turn ends.

**`job.kill`** — params `{job}`. Ends that run: a worker-owned command is killed, a pending
provider request is dropped, and the turn finishes with `turn.end {turn, aborted: true,
reason}` plus a durable `job.updated {job, state: "killed"}`. Answers `{killed}`; false when
the job was not running.

`turn.end` carries `aborted: true` only when a turn was killed rather than finishing its
driver.

**`subagent.prompt`** — params `{session, text}`. Forks the parent's log into a child
session, appends `subagent.updated {subagent, parent, state}` to the parent, runs one turn in
the child and answers `{session: child, turn}`. The child is an ordinary session: the same
`session.resume`, `session.cancel` and replay apply, and several children of one parent run
at once because each has its own turn.

**`subagent.interrupt`** — params `{session}`. Stops a child's running tool the way
`session.cancel` does and answers `{interrupted}`; false when nothing was running.

**`session.compact`** — params `{id}`. Asks the provider to summarise the conversation so
far, appends `session.compacted {through, summary}` and answers `{session, through, summary}`.
The history builder replays it: a later prompt sends the summary as one system message and
only the turns recorded after `through`. The summarise call is the one request still made
synchronously, because the answer is the method's result; it is bounded by the same timeout
as a turn.

**`session.checkpoint`** — params `{id, label?}`. Appends `session.checkpoint {label}` and
answers `{session, seq}`, the sequence a later branch can start from.

**`session.undo`** — params `{id, to?}`. Branches the session at `to`, or at the last
checkpoint when `to` is absent, and answers `{session}` for the new branch. The transcript is
append-only, so undo never rewrites: it forks the log, exactly as `session.fork` does.

**`session.rename`** — params `{id, name}`. Stores the label in the label database and
appends `session.updated` so subscribers see it. Answers `{session}`.

**`session.pin`** / **`session.archive`** — params `{id, pinned}` / `{id, archived}`, both
booleans. Same event and answer as rename. An archived session disappears from
`session.list` unless `include_archived` is true.

**`session.search`** — params `{query, limit?}`. Matches the name or the id, substring,
case-insensitively, newest first. Answers `{sessions: [...]}`.

A session object is `{id, cwd, model, created, last_seq}` plus, once it has a label row,
`{name, archived, pinned, updated_ms}`.

**`session.cancel`** — params `{id}`. Ends the session's turn wherever it waits: a worker
command is killed, a provider request is dropped, an approval is abandoned, and the turn
finishes with `turn.end {turn, aborted: true, reason}`. Answers `{cancelled}`; false when the
session was not running.

**`session.fork`** — params `{id, from?: uint}`. Copies the parent's durable events with
`seq <= from` (all of them when `from` is absent) into a new session with its own id and
sequence numbers, then appends `session.forked {from, at}` so the transcript records the
lineage. Answers `{session}`.

**`session.resume`** — params `{id: text, from?: uint}`. Attaches this connection to the
session's event stream under a freshly allocated `sub`, and returns
`{session: <session>, sub: <uint>, last_seq: <uint>}`. The response is queued before any
event, so the client knows its `sub` before events arrive. With `from`, every durable
event with `seq > from` is then sent on that `sub`, in order, followed by
`event kind replay.end` carrying `{from, to}` and `seq = to`. Without `from` there is no
replay and no `replay.end`.

A session object is `{id, cwd, model, created: <unix ms>, last_seq: <uint>}`.

### session.prompt

`session.prompt {id, text}` answers `{session, turn}`, where `turn` is the id that
`turn.start` and `turn.end` for this turn carry in their payload. A client waits for its own
turn's end rather than for the next event of that kind. It first appends `session.prompted {text}` — the log records what
was asked, not only what was answered — and then runs one turn in the session and answers
`{session}`. The
turn's events are appended to the session log before they are broadcast, so they reach
every subscribed client and survive a replay:

- `text.delta` — payload `{text}`; a piece of assistant text.
- `turn.end` — payload `{}`; the turn finished.

A prompt is synchronous: the request is answered after the turn ends, and the events
arrive before the response. A second prompt on the same session is served after the
first returns.

A turn that reaches a tool call the driver wants approved suspends instead of running it.
`session.prompt` then answers `{session, approval: {call, epoch, reason}}`, and the turn
resumes when `approval.respond` answers that pair. The rest of the turn's events arrive
after the response to `approval.respond`, before its `{resolved: true}`.

The three M1 driver commands are scripted stand-ins for a model's tool calls: `/read
<path>` runs the `read` tool, and `/write <path> <text>` runs `write`, which the driver
marks as needing approval. The rest of the prompt goes to the provider.

### approval.respond

`approval.respond {session, call, epoch, allow}` answers the parked request and resumes
the turn. The first valid answer wins; a second answers `already_resolved`, and an
unknown or expired pair answers `not_found`. Its events, all durable, are:

- `approval.requested` — payload `{call, epoch, reason}`; the turn is now parked.
- `approval.resolved` — payload `{call, epoch, allow}`; the answer, recorded before the
  turn continues.

Because both are durable, a client that attaches after the fact still sees the request on
replay and can answer it, provided it has not expired.

### Device pairing

**`device.pair`** — params `{scope?: text}`, default `desktop`, one of `readonly`, `phone`,
`desktop`. Local only. Mints a single-use enrolment token valid for five minutes and returns
`{token, fingerprint, endpoint, scope, expires_ms}`; a QR rendering of those fields is a
client concern.

**`device.enrol`** — params `{token: text, csr: text}`. Served only over QUIC, on a
connection with no client certificate. Redeems the token and signs the PEM certificate
request with the scope the token was minted for, answering `{cert, ca, serial}`; the
connection closes afterwards. An unknown, spent or expired token is `permission_denied`.

**`device.list`** — local only. Returns
`{devices: [{serial, name, scope, issued_ms, revoked}, ...]}`, oldest first.

**`device.revoke`** — params `{serial: text}`, local only. Marks the device revoked, rewrites
the CRL, closes any live connection using that certificate and answers `{revoked: true}`.

Scope is enforced by the daemon. A `readonly` device may call the read methods, but
`session.prompt`, `session.cancel` and `approval.respond` answer `permission_denied`. Device
administration is local only; a QUIC connection asking for it is `permission_denied`.

### The model catalog

The catalog is data, not code: `models.toml` in the state directory, one `[[model]]` table
per entry with `id`, `provider`, `model`, `context`, `tags` and `price` (all text; `price`
is `in,out` per million tokens). It is read at
start-up; a missing file is an empty catalog, and a duplicate `id` is a start-up error.

**`model.list`** — no params. Returns `{models: [{id, provider, model, context, tags}]}`.

**`model.select`** — params `{id}`. Makes that model the daemon's default for sessions that
do not name one, or `not_found`. Answers `{model}`.

**`session.set_model`** — params `{id, model}`. Numbers a durable `session.model {model}`
event, updates the session and answers `{session}`. The event is replayed when the session is
loaded, so the choice survives a restart. An unknown model is `not_found`.

`session.create` refuses a model the catalog does not hold when the catalog is not empty.

### Declared, not served in M0

The frozen list, from `README.md`:

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
terminal.{open,write,resize,close}
file.{upload,read}
module.{list,reload}
stats.{session,global}
```

An unknown or unserved method returns `not_supported`. A malformed param returns
`invalid_params`. Neither closes the connection.

## Events

Event bodies carry `kind` and `payload`; the frame's `sub` and `seq` carry routing and
order, so the logical event is `(sub, seq, kind, payload)`.

Served in M0:

- `session.updated` — payload `{session: <session>}`; durable. Emitted on create; later on
  rename, archive and pin.
- `replay.end` — payload `{from: <seq>, to: <seq>}`; not durable, not logged, does not
  consume a sequence number. Marks the end of a replay burst.

Served in M1:

- `text.delta` — payload `{text}`; durable.
- `turn.end` — payload `{}`; durable, emitted after the last delta of a turn.

Served later, all durable and all additive — a client that ignores them sees exactly the M1 stream:

- `turn.start` — payload `{turn}`.
- `turn.end` — payload `{turn, aborted?, reason?}`; `aborted` and `reason` appear only on an
  aborted turn.
- `turn.state` — payload `{turn, state}`; `prefilling`, `answering` or `idle`. Says what the
  turn is waiting for while it waits.
- `turn.timings` — payload `{prompt_n, prompt_ms, predicted_n, predicted_ms, cache_n}`; the
  provider's own numbers when it reports them, which is what makes a cache hit observable.
- `error` — payload `{code, name, message, turn}`; the frozen event, emitted before an
  aborted `turn.end` when the provider failed.
- `session.time` — payload `{text}`; the local time of a prompt, recorded so a replay
  reproduces the bytes the model saw.
- `session.head` — payload `{text}`; the frozen system prompt, appended once per session.
- `session.overflow` — payload `{bytes, budget}`; the assembled history exceeded the
  window, said rather than silently trimmed.
- `judge.decision` — payload `{tool, verdict, escalated}`.
- `session.modules`, `session.upgraded`, `session.migration_failed`, `session.attachment`,
  `session.compacted`, `session.forked`, `session.checkpoint`, `session.system`,
  `session.prompted`, `session.model`, `goal.set`, `goal.cleared`, `schedule.fired`,
  `job.updated`, `subagent.updated` — as their methods describe.

The full frozen list, from `README.md`:

```
turn.start  reasoning.delta  text.delta
tool.call   tool.result      approval.requested  approval.resolved
session.updated  context.compacted  job.updated  subagent.updated
terminal.output  judge.decision  error
```

## Sequences, replay and resync

`seq` is per session, starts at 1, and increases by one per event. Durable events are
appended to the session log before they are sent; volatile ones (deltas, `replay.end`) are
not. Consequently replay can show a gap where volatile events were, and that gap is
expected: a client must reconstruct volatile state from a snapshot, not from replay.

`replay` as a control frame re-sends durable events after `from` on an existing `sub`,
ending with `replay.end`. It exists for a client that dropped frames on a live socket;
`session.resume {from}` is the same mechanism at attach time.

`session.resync` is declared but not served in M0. It is the snapshot half of the pair: a
client that reconnects asks for a snapshot plus `last_seq`, rather than assuming replay
can rebuild volatile state.

## Errors

Responses use `error: {code, name, message}`. The numeric code is stable; clients should
key on `name`.

| code | name | meaning |
|---|---|---|
| 1 | `invalid_frame` | malformed frame, wrong direction, or a protocol violation |
| 2 | `version` | unsupported protocol version |
| 3 | `unauthenticated` | peer uid is not the daemon's uid |
| 4 | `not_found` | no such session |
| 5 | `invalid_params` | missing, mistyped or out-of-range params |
| 6 | `already_resolved` | an approval was answered twice |
| 7 | `busy` | the session cannot take this request now |
| 8 | `internal` | the daemon failed; the message says where |
| 9 | `not_supported` | declared but not served |
| 10 | `conflict` | the request contradicts existing state |
| 11 | `cancelled` | the request was cancelled |
| 12 | `limit` | a size or concurrency limit was hit |
| 13 | `permission_denied` | the device scope or the transport does not allow this |

The daemon sends an error response when it can, and closes only on `invalid_frame`,
`version` or `unauthenticated`. Before the handshake completes there is no request to
answer, so those errors use `id: 0`; `0` is otherwise reserved and never valid as a
request id.

## Session log

`sessions/<id>/events.zst` is a sequence of records, each `u32 len` little-endian
followed by exactly one zstd frame of `len` bytes. The decompressed bytes of a record are
one CBOR map.

- Record 0 is the header: `{v: 1, id, cwd, created, model}`. It is authoritative for
  existence; the directory name must equal `id`.
- Every later record is a durable event: `{seq, kind, payload}`.

The log is append-only and never rewritten. Reading stops at the first record that does
not decode, and the reader reports how many bytes decoded: a torn tail from a crash is
expected, corruption in the middle is not.

## Versioning rules

`hello.v` and `welcome.v` are `1`. Adding a method, an event kind, an error name or a map
key is a compatible change and does not bump the version. Renaming or removing one, or
changing the frame layout, does. There is no negotiating below 1: a peer that does not
send `v: 1` is refused.
