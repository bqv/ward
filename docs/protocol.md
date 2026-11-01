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
method list below — prompts, tools, approvals, subagents, jobs, schedules, goals, settings,
terminal, file transfer, modules, stats. QUIC, device certificates, session tokens and
message chunking arrive with M1/M4; until then the frame body cap is 64 KiB and a larger
message is `limit`.

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

**`session.resume`** — params `{id: text, from?: uint}`. Attaches this connection to the
session's event stream under a freshly allocated `sub`, and returns
`{session: <session>, sub: <uint>, last_seq: <uint>}`. The response is queued before any
event, so the client knows its `sub` before events arrive. With `from`, every durable
event with `seq > from` is then sent on that `sub`, in order, followed by
`event kind replay.end` carrying `{from, to}` and `seq = to`. Without `from` there is no
replay and no `replay.end`.

A session object is `{id, cwd, model, created: <unix ms>, last_seq: <uint>}`.

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
