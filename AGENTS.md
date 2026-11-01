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

Eighth: the rest of the M1 surface. The driver asks `clock.now` and stamps every prompt with
the local time (`[2026-11-01 18:28:47 EET] hello`), so the model sees when it is; `ward -p
TEXT` creates a session if needed, runs one turn and prints it, which is the headless entry
point; and the model catalog is the next slice.

Two more ABI notes: a CBOR integer wider than 32 bits is skipped rather than refused —
`clock.now` returns a millisecond timestamp and the guests that only skip it must not choke —
and `wd_r_uint` still truncates such a value, so a guest that needs the whole 64 bits reads
the text form instead.

Ninth: the model catalog. `src/core/models.c` reads `models.toml` from the state directory —
one `[[model]]` table per entry, text fields, a duplicate id refused, a missing file an empty
catalog — and the daemon serves `model.list`, `model.select` and `session.set_model`.
Selecting sets the default for new sessions; setting a session's model appends a durable
`session.model` event, and the loader replays the log so the choice survives a restart.
`ward model list`, `ward model select` and `ward session set-model` are the client side.

Tenth: the worker. `src/core/worker.c` forks one supervisor per tool root on first use: the
child calls `setsid`, sets `PR_SET_PDEATHSIG` so core death takes it and its children, and
serves a small binary request protocol — argv, timeout, byte cap — running each command as
its own process group and replying with the wait status, flags and output. A worker that has
died is replaced by the next call, which is the containment the design asks for; exec no
longer runs as a direct child of the daemon.

Eleventh: `session.fork`. The copy is over the log, not over memory: `wd_session_fork` creates a
fresh session with the parent's cwd and model, replays the parent's durable events up to the
chosen sequence, re-emits them under the child's own sequence numbering and appends
`session.forked {from, at}` so the lineage is in the transcript rather than only in a table.
`session.fork` serves it and `ward session fork <id> [--from SEQ]` drives it; the tool gate
forks a session that has run a turn, checks the child's sequence and replays the child's
`turn.end`.

Twelfth: tools left the event loop. `modules/tools` is now pure — it only maps `{tool, args}`
to `{call, payload}`, the capability that performs it — so core can decide *how* to perform
it. Read, write, edit and grep are still called inline; exec is submitted to the worker and
the turn parks, exactly as it parks for an approval. The daemon watches the worker's fd in
epoll and resumes the turn when the reply is readable, so the loop serves other clients
while a command runs. `gate_tool` proves it: it approves a `/run sleep 1`, immediately asks
for `session.list` and gets an answer before `turn.end` arrives.

The provider request is the one blocking call left: `net.request` runs curl inside the
daemon's loop, so a slow model still stalls it. That is the next thing to lift, and the
parking machinery is now in place to do it.

Thirteenth: `session.cancel`. With a tool in a worker there is something to stop: the method
kills that worker, the daemon sees its socket close, the turn takes the failure as the tool
result and finishes with `turn.end`. The daemon gate starts `/run sleep 5`, approves it,
cancels it and asserts `turn.end` arrives in under three seconds — which also proves the
turn is genuinely parked rather than the loop being busy.

The bug that first made this hang is worth remembering: the resume must complete the worker
it submitted to, not whatever `worker_for` returns afterwards, because a killed worker looks
exactly like a worker that needs replacing and the reply would then be awaited on a socket
that was never written to.

Fourteenth: M2's acceptance, measured. `gate_crash` runs a turn to completion, parks a
second one on an approval, kills the daemon with SIGKILL, restarts it on the same state and
replays from zero: every durable event is there, sequence numbers are contiguous from 1 to
`last_seq`, `turn.end` and `approval.requested` both survived, the lock file was reclaimed
from the dead pid, and a new prompt runs against the restarted daemon.

Writing it found a small but real bug: `wd_store_open` scanned every directory under the state
root as a session, so `cache/` and `pki/` produced "skipping unreadable session" warnings on
every start. It now treats only a 16-hex-character name as a session id.

Fifteenth: session labels in SQLite. `src/core/labels.c` keeps `<state>/labels.db` with one
row per session — `name`, `archived`, `pinned`, `updated_ms` — and the daemon serves
`session.rename`, `session.pin`, `session.archive` and `session.search`, each appending a
durable `session.updated` and answering the labelled session object. `session.list` hides
archived sessions unless `include_archived` is set, and the session object now carries the
label fields whenever a row exists. `ward session rename|pin|archive|search` are the client
side, and `session list` prints the name and the pin/archive state.

A trap worth more than the feature: **libcbor's definite containers are fixed-size.**
Building a `wd_map(5)`, then adding four more keys, encodes five and silently drops the
rest — the label fields vanished from every response while the daemon log showed them being
read. `session_cbor` now builds the map at its exact size, and `session.list` counts first
and then allocates the array, because the same bug bites `cbor_array_push` on a `wd_array(0)`.

Sixteenth: usage stats. `wd_session_measure` replays a session's log and counts its events, its
`turn.end`s and the bytes of its text deltas; `stats.session` and `stats.global` serve the
counters and `ward stats [<id>]` prints them. No tokenizer yet, so this is the shape cost
accounting will price rather than a price — the numbers are exact, the money is absent.

Seventeenth: the provider left the loop too. `modules/provider` is two-phase like `tools`:
`{prompt}` in gives `{request: {path, body}}` out, and the second call takes
`{status, body}` and gives `{text}`. Core performs the request through a forked helper —
`wd_http_start` runs the existing curl call in a child and hands the parent a pipe — so the
daemon watches that pipe in epoll, parks the turn and resumes it when the reply is readable.
Nothing on the prompt path blocks the loop now.

M1 is feature-complete: no item on its parity row is missing, and both of its acceptance
lines are gated.

Eighteenth: checkpoints and undo. `session.checkpoint {id, label?}` appends a
`session.checkpoint` event and answers its sequence; `session.undo {id, to?}` branches the
session at that sequence, or at the last checkpoint when asked for none, through the same
`wd_session_fork` the fork method uses. Undo is a branch rather than a rewrite because the log is
append-only — the old line stays, and the client gets a new session id whose transcript is
the history up to the checkpoint.

Nineteenth: the log records the question. `session.prompt` appends `session.prompted {text}`
before running the turn, so a transcript can be read back as a conversation rather than as
answers to questions nobody kept. `wd_session_measure` counts those bytes as `prompt_bytes`, and
both stats methods report it beside `text_bytes`.

This is the foundation the two remaining M2 rows need: compaction summarises what was asked,
and a price needs both sides of it. Until this commit the user's own words were not durable
anywhere, which would have made either row a guess.

Twentieth: history. `wd_session_history` replays a log into `[{role, text}]` pairs — a
`session.prompted` opens a user turn and the deltas up to `turn.end` close an assistant one —
and trims from the front to a byte budget. The daemon sets it on the turn before each
prompt, core merges it into the provider payload and `modules/provider` writes the messages
in order, so a conversation is answered in context rather than one prompt at a time. The
fake endpoint now echoes the *newest* content, so the gates still assert the current prompt
while the history rides along.

Twenty-first: cost accounting. A model entry may carry `price = "in,out"` per million
tokens; `stats.session` looks the session's model up in the catalog and answers
`{cost_in, cost_out, cost_total}` beside the counters, and `ward stats` prints the total.
The token count is bytes over four and the answer says so with `tokens_estimated: true` —
a labelled estimate, not a measured price, until the tokenizer the README reserves a core
budget for exists. The field shape will not change when it does.

Twenty-second: compaction. `session.compact {id}` builds the session's history, asks the
provider through the same guest and job machinery a turn uses — `wd_turn_compact` runs it
with a fixed summarise instruction — appends `session.compacted {through, summary}` and
answers the summary. `wd_session_history` honours it: replaying that event throws away the turns
collected so far and starts again from a single `system` message, so later prompts send the
summary plus whatever happened after it.

This is the one method that still waits: the summary is its result, so it is fetched
synchronously with the turn's timeout rather than parked. Everything else on the prompt path
parks.

Twenty-third: one prefix. The `hs_` and `wu_` prefixes were the last survivors of an older
split, so the session store and the utility helpers moved to `wd_` with the second segment
naming the owner: `wd_store_open`, `wd_session_emit`, `wd_session_history`, `wd_path_join`,
`wd_now_ms`. Every exported core symbol is now `wd_*`; nothing outside a header carries a
prefix at all. It is a rename with no behaviour change, which is why the same 21 tests pass
before and after.

Twenty-fourth: turn ids. A turn is numbered when it starts, and core stamps `turn.start` and
`turn.end` with that number in the payload; `session.prompt` answers `{session, turn}`. The
tool gate now waits for the end of the turn it started, which is what its old `wait_event`
should always have meant — before this it could consume a previous turn's end, and that is
exactly how the M1/M2 daemon's habit of clobbering a parked turn stayed invisible.

Twenty-fifth: the run pool. The daemon no longer owns one `wd_turn`: `run_new` gives each
prompt its own turn, `run_find` keys them by session, and the epoll tokens name the run they
belong to, so a worker or provider completion resumes the turn that asked for it. A session
with a live run answers `conflict: the session is already busy` rather than clobbering it,
which is the bug the first attempt found. Compaction borrows a run and frees it.

Twenty-sixth: subagents. `subagent.prompt {session, text}` forks the parent's log into a
child session, appends `subagent.updated` to the parent and hands the child to the same
prompt path — so a child is an ordinary session with its own run, its own worker and its own
replay. `subagent.interrupt` finds the child's run and cancels its tool.

The tool gate measures the thing the milestone asks for: the fake endpoint now delays each
reply and serves connections in a forked child, two subagents start back to back, and the
gate asserts both turns finish inside 700 ms when a single turn takes 400 — 466 ms measured,
so they were genuinely in flight together rather than merely started.

Twenty-seventh: what `job.kill` must mean. A first attempt exposed the running set through
`job.list` and made `job.kill` cancel the run's worker, mirroring `session.cancel`. It hung
in the gate, and the reason is structural rather than a slip: `session.cancel` stops a
*command*, so it only applies to a turn parked on the worker. A job can equally be parked on
the provider — there is no worker to kill — and killing it has to end the run, which means
`turn.end` with an aborted result and a `job.updated {state: "killed"}` notice, not a worker
kill that may never come. The slice was reverted rather than committed untested; the next
attempt should start from that distinction: cancel stops a command, kill ends a run.

Twenty-eighth: abort, then jobs. `wd_turn_abort` is the primitive the previous round lacked:
it ends a parked turn wherever it is waiting — a worker command is killed, a provider reply
is dropped, an approval is abandoned — and emits `turn.end {turn, aborted, reason}` itself
rather than re-entering the driver, whose state no longer matches. `job.list` reports the
runs, `job.kill` aborts one and appends `job.updated {state: "killed"}`, and
`subagent.interrupt` now uses the same primitive, so it also stops a child waiting on the
provider, which `session.cancel` cannot.

The gate proves it on the case that hung last round: two subagents are in flight, the second
is killed while parked on the provider, its `turn.end` comes back `aborted`, and the first
still finishes inside 700 ms — 488 ms measured.

Twenty-ninth: the schedule module, and how not to test a guest. The cron arithmetic belongs in
a guest — core owns only the wheel — so a `schedule` module answering `{cron, from_ms}` with
`{next_ms}` is the right shape, and it compiled. The test that drove it directly segfaulted
before asserting anything: a guest instance needs `wd_engine_new` with a real state directory
and an explicit `wd_limits`, not the NULLs that look reasonable in a small test. The slice was
reverted rather than committed half-tested; the next attempt should model its test on
`wasm_test`'s loader (`read_file`, `wd_module_load`, a named limits struct) and only then add
the daemon's `schedule.set/list/delete` and the wheel on top.

Thirtieth: goals. `goal.set` and `goal.clear` append durable events, and the history builder
prepends the active goal as a `system` message to every later prompt — so it survives a
restart by replay rather than by a table, and the model is reminded what the session is for
without the client repeating it. `hs_history` tracks the goal through the replay: set, cleared,
or replaced.

Thirty-first: the plan gate. `/plan <text>` asks the policy guest for `plan`, which answers
`ask`, and the driver invokes the `plan` tool exactly as it invokes `read` or `exec` — so the
turn parks on an approval with that reason, and `plan.run` in core answers the approved text.
No new core machinery: the gate is a policy row, a tool mapping and a driver branch, which is
what it should be. The tool gate prompts a plan, approves it and waits for the turn's own end.

Thirty-second: cancel ends a turn too. `session.cancel` was a worker kill, so it did nothing
for a turn waiting on a provider reply — the same gap `job.kill` had before `wd_turn_abort`
existed. It now uses the abort primitive: any parked turn can be cancelled, the client gets
`turn.end {turn, aborted, reason}`, and the run is freed. The tool gate cancels a
provider-parked subagent and checks the aborted end, which is the case the old method could
not touch.

Thirty-third: the schedule guest, second attempt. The test now runs — `wd_engine_new(NULL)` is
fine, the mistake was passing NULL limits — and it found a real bug in the guest: a helper
wrote its digits into the reply buffer that the CBOR writer was about to fill, so the answer
came back as raw ASCII and `wd_decode` refused it. After that fix the guest answers a valid
map but finds no match for `*/5 * * * *`, so the search or the field parse is still wrong; the
slice was reverted again rather than committed failing.

The lesson is the one worth keeping: a guest test needs the loader pattern from `wasm_test`
(`read_file`, a named `wd_limits`, `wd_engine_new(NULL)` is fine) and the reply buffer is
written by `wd_w_*` and nothing else — scratch text belongs in a local array.

Thirty-fourth: the job client. `ward job list` prints the running runs and `ward job kill <id>`
aborts one, so the two methods the gate proves through the protocol are reachable from the
client as well. The CLI is a thin client of tested methods here rather than a new surface;
the gate remains the authority on behaviour.

Thirty-fifth: schedules. `modules/schedule` turns a cron expression — five fields with `*`,
numbers and `*/n`, plus `@every <ms>` — into `{next_ms}`, and core holds only the entries and
the timer: `schedule.set` stores one, `schedule.list` shows them, `schedule.delete` removes
one, and the loop caps its epoll timeout by the earliest fire. When one is due the daemon forks
the session, runs the text as a turn in the child, appends `schedule.fired` and re-arms.

The bug that cost two rounds was in the *test*: cron semantics are "strictly after now", and
the guest was returning the current minute when it matched, so `next > from` failed and looked
like a broken search. A guest test is worth its cost twice over here: it found that, the
reply-buffer clobber behind it, and then an out-of-range field (`61`) that scanned four years
of minutes until it ran out of fuel — now rejected outright.

Thirty-sixth: the job queue. The daemon caps concurrent turns (`WARD_MAX_RUNS`, eight by
default). A background turn — a subagent or a schedule fire — started past the cap is queued
rather than refused: it answers `{session, queued: true}`, `job.list` reports it as `queued`,
`job.kill` can drop it before it runs, and finishing a run drains the queue in order. The
gate proves it with a cap of two: two subagents run, a third queues, the list shows exactly
one queued entry, and the third's turn ends once the others are done.

M3 is complete: every row on its parity line is delivered, and the acceptance gate — two
subagents concurrently under one session — passes in all three builds.

Thirty-seventh: the skills guest. A prompt starting `/` that no tool command claims is offered
to `modules/skills` through the `skills.expand` capability, which answers `{handled, text}`;
`/help`, `/rules` and `/skills` are its own, and anything unhandled falls through to the
model. The guest is pure — no imports — so the driver can reach it only through core, and the
tool gate prompts `/help` and asserts the guest's answer arrives as a text delta.

A first attempt also prepended the repository's AGENTS.md to every prompt. It was withdrawn:
the driver's fixed 512-byte text buffer and 2 KiB reply cannot carry a real rules file, and a
300-byte truncation is not "repo rules load". That row needs the rules delivered as a history
system message with its own budget, which is the next M4 slice.

Thirty-eighth: rules as a system message, and a dead end worth recording. The second attempt
at AGENTS.md put the rules where they belong — a `system` entry prepended to the turn's
history rather than text glued onto the prompt — and raised the provider's buffers so a rules
file plus a transcript fits (16 KiB JSON, 24 KiB reply, 1 MiB heap). The daemon read the file
(a log line confirmed 21 bytes from the right root) and the history was built with it, but the
request that reached the fake endpoint carried only the stamped prompt. The slice is reverted
with the mechanism half-understood: the next attempt should start by printing the provider's
payload at the point `process_action` merges the history, because the fault is between the
merge and the guest, not in the reading.

The provider buffer increase is kept: a 4 KiB JSON buffer could not carry a rules message and
a transcript together, and that limit would have bitten regardless.

Thirty-ninth: web.search, first attempt, and the mistake to avoid. Core grew the capability —
`{query, k}` in, a `srchdsh search -q … -max … -json` child run with `wd_proc_run` and its
output returned as `{ok, status, text}` — and the skills guest was taught to call it for
`/search`. It hung the gate instead of working, and the likely cause is structural: `skills`
was loaded as a *pure* module (`no_caps, 0, true`), so the moment it gained a `ward.cap`
import the host refused or mis-instantiated it, and every prompt after that stalled.

The fix for the next attempt is explicit: a module that calls a capability is not pure. Its
spec must say `false`, its granted list must name `web.search`, and the instance must be
created with those grants — the same shape `driver` already has. Read the host's import
enforcement before adding an import to a guest that was pure yesterday.

Fortieth: web.search. The skills guest calls core's `web.search` for `/search <terms>`;
core runs the local `srchdsh` binary as a child (`search -q … -max … -json`, no shell, 20 s,
64 KiB) and answers `{ok, status, text}`. The module owns the judgement — what to search for,
which result to trust — and core owns only the child process, which is exactly the split the
README asks for.

The previous round's hang was the pure-module spec: `skills` is loaded `false` with
`{"web.search"}` granted now, and the same code that stalled then passes. Two lessons kept:
a guest that gains an import is no longer pure, and the gate's stub binary (`WARD_SEARCH_BIN`)
keeps the test off the network.

Forty-first: the MCP client. `mcp.call {request}` runs the configured server binary
(`WARD_MCP_BIN`, else `~/bin/mcp`), writes one JSON-RPC line to its stdin, closes it and
returns the first response line as `{ok, status, text}`. `src/core/proc.c` grew
`wd_proc_run_input` for that — one child, a stdin pipe, the same bounded drain as `exec` —
and the skills guest answers `/mcp <json>` with what the server said, so the module still
decides and core still only owns the process.

One request per process is the M4 shape and the note says so: a persistent session with
`initialize`/`tools/list`/`tools/call` and a framed transport is a later milestone's work.

Forty-second: the rules row, narrowest reproduction. With `history_with_rules` back in and a
`WARD_DEBUG` line in `process_action`, the daemon reports `provider history=yes size=1` for a
turn whose root holds an AGENTS.md — so core builds the history correctly. A debug write
inside the provider guest (`fd_write` on the `history` key) never fires, so the guest never
sees the key even though core passed it two statements earlier. The gap is therefore in
`provider_phase`/`invoke` encoding the merged payload, not in reading the file, not in the
session history, and not in the provider's JSON writer.

That is the whole of the next attempt: read `invoke()` and how it encodes a *borrowed* map,
with `wd_encode`'s consume-on-encode rule in mind. The diagnostics are reverted again; the
finding is the deliverable.

Forty-third: the provider test. `tests/provider_test` loads the provider guest directly and feeds
it `{prompt, history: [{role: "system", text: "..."}]}`, then asserts the JSON body it builds
carries both the rules and the prompt. It passes — so the guest handles a history array
correctly, and the rules failure in the daemon is *not* the provider's parsing or its JSON
writer.

That leaves the daemon's own call path: core builds and merges the payload with the rules
(the debug line showed `keys=2 hist=1`), the guest is proven good, and the request that arrives
at the endpoint still carries only the prompt. The next probe belongs one layer down — print
the encoded bytes handed to `wd_instance_call` for the *first* turn, not a later one, because
the debug line only ever appeared once in a run.

Forty-fourth: the rules row, closed — and a harness that hid the answer for four rounds. The
mechanism was right from the round the provider buffers were raised: core reads
`<cwd>/AGENTS.md`, prepends it to the turn history as a `system` message, and the provider
sends it. Every failure since was the *test*: the fake endpoint echoed only the **last**
content, so the user prompt was all a gate could ever see, and the absence of the rules looked
like a broken feature. The endpoint now echoes the last content and appends `[rules]` when the
marker was somewhere in the request, and the gate asserts that.

Two lessons, both expensive: a stub that reports only the tail of a request cannot prove
anything about its head, and two in-process tests had been running their turns in the
repository root — where the rules now apply — so they were moved to an empty root, which is
what a test should have used from the start.

Forty-fifth: the PTY primitive. `src/core/pty.c` opens a child under a pseudo-terminal —
`forkpty` with a window size, the command exec'd directly with a scrubbed environment and a
`TERM`, the master end non-blocking — and offers write, resize (`TIOCSWINSZ`), a read and a
close that hangs the process group up before the kill. `tests/pty_test` opens `cat`, resizes
the window, writes a line and reads it back through the terminal, which is the whole contract
in one test.

This is the core half of the terminal row: authority over a descriptor and a child. The daemon
side — `terminal.{open,write,resize,close}` and the `terminal.output` event the frozen
protocol already names, with its own epoll token and a `wd_term` table on the server — is the
next slice, and it is deliberately not bolted on in the same round as the primitive that it
needs to be correct.

Forty-sixth: terminals in the daemon. `terminal.open` forks the command under a pty in the
session's cwd, registers the master fd in epoll against its own token range, and answers
`{terminal, pid}`; write, resize and close complete the quartet the frozen protocol names. On
readable the loop pumps up to 4 KiB into a `terminal.output {terminal, data}` broadcast, and on
EOF it announces `{eof: true}` and drops the handle.

That event is deliberately **not durable**: terminal bytes are unbounded — `yes` in a shell
would grow a session log without limit — so `terminal.output` goes to subscribers only and the
protocol note says so. The gate opens `cat`, writes a line, asserts the line comes back through
the terminal, resizes, and closes.

Forty-seventh: recall. `recall.search {query, k}` runs the local `dsh-recall search <query> -k
<k>` as a child and returns its words; the skills guest answers `/recall <words>` with them.
The index, the embedding endpoint and the ranking are all `dsh-recall`'s — core owns the child
process and nothing else, which is the same shape as `web.search` and `mcp.call`.

The gate points `WARD_RECALL_BIN` at a stub and asserts its line arrives as a text delta, so
the row is measured without depending on a live index.

Worth fixing while it is fresh: `web.search` defaults to `~/bin/srchdsh`, but the binary is at
`~/.local/bin/srchdsh` on this box. `WARD_SEARCH_BIN` hides it in tests; the default needs
checking against the real path when search is next touched.

Forty-eighth: attachments. `attachment.add {id, name, data}` stores the bytes under
`<state>/<session>/attachments/<name>` — refusing a name with `/` or `..`, capped at 4 MiB —
appends a durable `session.attachment {name, size}` and answers the path. `attachment.list`
replays those events into the list. The gate adds a note, checks the path, and lists exactly
one entry.

The trap that cost the round is the one already written down: an array built with `wd_array(0)`
and filled by `cbor_array_push` stays empty, because a definite array is fixed-size. The
collector now grows a plain `cbor_item_t **` and the reply array is allocated at its final
size. It is the second time this exact bug has been the whole delay; the note in the libcbor
section is the one to read first when an array comes back empty.

Attachments are stored, listed and durable. They are **not** yet shown to the model — a prompt
does not mention them — which is the next step for the row rather than something this round
claims.

Forty-ninth: reading a turn, not a delta. The gate asserted on the first `text.delta` after a
prompt, but the fake endpoint splits its echo into two chunks, so every turn emitted two deltas
and the gate consumed one. The leftover sat in the queue and the *next* assertion popped it —
which is how a working rules row and a working attachment context both looked broken. `turn_text`
now collects a turn's deltas until that turn's own `turn.end`, and the rules assertion uses it.

The larger lesson is the one from round 44 in a new costume: an instrument that reads the wrong
sample produces a false negative that looks exactly like a bug in the code under test.

Fiftieth: the attachment context. A turn now carries up to two `system` entries before the
history: the project rules and a line naming the session's attachments — `attached to this
session: note.txt (13 bytes)` — built by replaying `session.attachment` events when the run is
created. `history_with_context` builds both in one pass, so the ordering is fixed and the array
is sized once.

The gate asserts it with the `[attach]` probe the endpoint adds when the name appears anywhere
in the request, read through `turn_text`. Placing that assertion *after* the subagent block
matters: an extra prompt in the middle changed how many runs were in flight and broke the queue
assertion, which is a reminder that this gate is a sequence, not a set of independent tests.

Fifty-first: the judge's contract. Before any model is consulted, the part the README says core
owns — the merge, the cache, the fail-closed rule — is now `src/core/judge.c`. `wd_verdict_max`
is the monotonic merge, so a judge can raise `allow` to `ask` and never lower a `deny`;
`wd_judge_decide` consults the judge only for an `allow`, keys the cache on `(tool, raw
argument bytes)`, remembers the answer, and falls back to the rule whenever the ask fails or
answers out of range. A `deny` never reaches the model at all, which is the property worth
having before any guest exists.

`tests/judge_test` measures all of it: the merge in both directions, a cache hit consulting the
model exactly once, a different argument missing the cache, a `deny` short-circuiting, and a
failing ask leaving the rule standing.

The guest — `judge.classify` over its own scoped endpoint, and the wiring into the policy gate
that emits `judge.decision` — is the next slice. Splitting it this way means the merge and the
cache are proven before a model is in the loop.

Fifty-second: the judge guest. `modules/judge` is pure — no imports — and answers
`{tool, payload}` with `{verdict, reason}`: it looks for destructive, network or remote
patterns in the payload and asks for approval when it finds one, allows the rest. `judge_guest_test`
loads the module the way `provider_test` does and asserts three verdicts directly, so the
classifier is proven without a turn, a policy gate or an endpoint.

This is the decision half of judge-assisted policy. The wiring — `judge.classify` through a
capability, `wd_judge_decide` merging the verdict, and a `judge.decision` event carrying model,
prompt hash, latency and verdict — is the next slice. The guest is rule-shaped today; when the
model-backed judge arrives it keeps this ABI and replaces the body with a `net` call, which is
the same shape `skills` took for search.

Fifty-third: the judge in the policy path. `policy.check` now asks the rule guest first and, when
it answers `allow`, hands `{tool, payload}` to the `judge` guest and merges the verdict through
`wd_judge_decide`. The driver sends the payload with the tool, so the classifier sees the words
it is judging rather than a bare name — `/read rm -rf everything` is the case the gate uses: the
rule allows `read`, the judge raises it to `ask`, and the client gets an ordinary
`approval.requested`. Denying it ends the turn; a plain `/read` still passes untouched.

Two pieces are still missing from the README's judge row and the note says so: the
`judge.decision` event (model, prompt hash, latency, verdict) is not emitted yet — `policy.check`
is a capability and has no emit path — and the guest is still rule-shaped. The merge, the cache
and the fail-closed rule are core's and are tested; the audit event and the model-backed guest
are the next two steps.

Fifty-fourth: python, in the honest form. `/py <program>` runs `python3 -I -c <program>` in the
session's cwd through the worker's child runner — 15 s, 64 KiB — and the gate proves the
acceptance line: `/py print(6*7)` asks for approval, and once approved the turn carries `42`.

It is **not** the embedded-CPython guest the design describes. There is no wasip1 CPython on
this box and building one is a project of its own, so the row ships as the host interpreter in
the worker, behind an approval, with the protocol note saying exactly that. The method, the
capability and the policy row survive when the guest arrives; only the child changes.

The gate also needed a longer endpoint delay: with three seconds per reply the two subagents are
still in flight when the third is issued, so the queue assertion measures the cap rather than
the machine's speed. That number had been drifting for several rounds and is no longer on the
edge.

Fifty-fifth: the judge's audit trail. When the judge is consulted, `policy.check` stashes the
outcome on the turn and the drive loop emits `judge.decision {tool, verdict, escalated}` after
the driver returns — the capability itself has no emit path, and giving it one would have meant
threading a callback through every capability for one event. The gate asserts the event: tool
`read`, `escalated: true`, on the turn that parked.

The judge row is now complete at the rule level: contract, classifier, escalation and audit.
What remains is the model behind the classifier, which is a swap of the guest's body rather than
new machinery.

Fifty-sixth: a corrupted module in the build directory, and what it cost. An abandoned experiment
overwrote `out/judge.wasm` with 17 bytes of text and died before restoring it — the test it was
part of had `alarm(120)` and the run was killed. From then on every gate hung with no output at
all: the daemon could not create a turn, so no event ever arrived, and the failure looked like a
test deadlock rather than a broken file. `make` would not rebuild it either, because the
corrupted file was newer than its source. `rm out/judge.wasm && make` fixed it and the suite went
back to green.

Two rules follow. A test must never mutate the module directory the suite shares: copy the
modules into the test's own temporary directory and point a second daemon at the copy — this is
also the first slice of M5, where the registry's reload path needs a throwaway tree. And when a
gate hangs with *no* output, suspect the build tree before the code: check that every
`out/*.wasm` is a real module (`head -c 8`) before reaching for a debugger.

Fifty-seventh: the M5 plan, and the line that cannot yet be met. A read-only survey of the tree
against the README produced two findings worth keeping before any M5 code is written.

First, **the M5 acceptance line is currently unfalsifiable.** `wd_turn_new` reads and instantiates
all seven guests on every prompt and `wd_turn_free` destroys them at `turn.end`; there are no
refcounts, no instance pool, and the only state that survives a boundary is core's own
`turn->parked` CBOR. So "upgrade a stateful module mid-session with no data loss" has no stateful
module to demonstrate it — a test written today would prove core to core. The honest way through
is the README's own conformance guest (export, import, migrate, trap), with a note that no
shipped guest carries state yet.

Second, **`ward_state_migrate` does not exist in the host**, although `docs/modules.md` freezes
it. `wd_instance_state_export` and `wd_instance_state_import` are implemented; the migrate call,
its schema argument and its `{ptr, len}` reply are not, so the upgrade order the README describes
has a hole exactly where the interesting case is.

The slices, in order, each building and committing alone:

1. **Registry identity** — a manifest per module (version, `state_schema`, capabilities), `@version`
   filenames, `wd_registry_resolve`, and an honest `module.list`/`module.reload` whose arrays are
   sized at their final length. Purity and grants stay in a core table: a manifest must never
   widen its own authority.
2. **Host `state_migrate`** plus a conformance guest — pure addition, no ABI change.
3. **Pinning** — a durable `session.modules` event, replayed like `session.model`, so a session
   keeps the versions it started with.
4. **`session.upgrade`** — the pin moves at a boundary, appending `session.upgraded`.
5. **Drain and migrate** — export, drop, instantiate, import, migrate between schemas; on failure
   keep the old instance and record `migration_failed`.
6. **Bad version** — repeated traps demote a version until cleared.
7. **`code.outline`** — the module decides what a symbol is, core reads the file under the tool
   root. Label it a scanner until tree-sitter is really there.
8. **The index** — `symbols`/`edges` sidecars in `cache/`, `code.defs` carrying a generation; this
   is the slice that meets `code.defs finds a symbol in the harness`.

Steps 1 and 2 are independent of the rest and can run first; 3–6 are a chain; 7–8 are orthogonal
and can start in parallel.

Fifty-eighth: the module manifest loader, slice one of M5. `src/core/manifest.c` reads
`<dir>/<name>.toml` — `name`, `version`, `state_schema`, `lazy`, `capabilities` — with the
same hand-rolled TOML idiom the model catalog uses, because the tree has no TOML library and a
manifest is five keys. It refuses a manifest whose name does not match its file, a capability
list that is not closed or too long, and a schema that is not a number; a missing `version`
means `"1"`, a missing `state_schema` means `1`.

`wd_manifest_compare_version` compares numerically first and falls back to text, so `2 < 10` as
an operator expects rather than as `strcmp` would say.

This is deliberately only the loader: the registry that scans, resolves and validates against a
core-owned table of purity and grants is the next step, and it needs this underneath it. Purity
and grants stay out of the manifest on purpose — a file a client can edit must never widen its
own authority.

## M3: started, with one finding worth keeping

The first M3 attempt replaced the daemon's single `wd_turn` with a pool keyed by session, so
two sessions — a parent and two subagents — can run turns at once. It built, and it exposed a
bug that the old design was hiding: **a second `session.prompt` while a turn was parked used
to clobber that turn silently**, because there was one turn object and no busy check. Every
gate that "waited for `turn.end`" after an approval had in fact been reading the *previous*
turn's end on occasion; with a per-session pool the daemon correctly refused the second
prompt with `conflict`, and the tool gate failed.

That is the shape of the real work, not a reason to go back:

- turns need identities: `turn.start` and `turn.end` should carry a turn id, and a client
  waits for the id it was given rather than for the next event of that kind;
- `session.prompt` must refuse a busy session with `conflict`, and the CLI must say so;
- then the pool goes back in, a session per run, and subagents are child sessions driven
  through the same pool.

The pool changes are reverted for now so master stays green; the finding above is the first
thing the next attempt needs.

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
