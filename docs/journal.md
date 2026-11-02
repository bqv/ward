# The ward journal

The running log of what was built, in order, with the traps that cost time. It was moved out of
`AGENTS.md` when that file grew past the instruction budget: the guide is what an agent needs
every turn, the history is what it needs when it is unsure why something is the way it is.

Earlier entries live in git history before the split; from here the journal grows at the end.

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

Fifty-ninth: the registry. `src/core/registry.c` scans a directory, keys one entry per
`(name, version)` — `echo.wasm` is version `1`, `echo@2.wasm` is version `2` — records a hash
and a size, and marks an entry `changed` only when the bytes differ from what was seen before. A
first sighting is new, not changed; that distinction is what makes a reload report meaningful
rather than noisy. `wd_registry_resolve` answers the highest valid version, or a named one;
an invalid file is skipped, so a corrupt `echo@2` falls back to `echo` instead of breaking
everything.

`wd_registry_validate` checks the wasm container (`\0asm` and version 1) and refuses anything
else. It validates the format, not the semantics — the honest limit is written down rather than
implied.

`tests/registry_test` copies real modules into a temporary directory and measures it all: two
versions, resolution to the highest, a named version, a missing version, an unchanged rescan
reporting zero changes, a corrupted v2 marked invalid with its error, and resolution falling
back to v1. The daemon side — `module.list` and `module.reload` on top of this — is the next
slice, and its gate will point a second daemon at a copied module directory, never at `out/`.

Sixtieth: module.list and module.reload. The daemon scans its module directory at start-up and
serves the registry: `module.list` answers `{name, version, size, hash, changed, valid}` per
entry, and `module.reload` rescans, validates whatever changed, and answers `{updated, refused}`
— a refused name with its version and the reason. Both arrays are allocated at their final size
after a growing pointer array, because `wd_array(0)` plus `cbor_array_push` is the trap this
repository has paid for twice.

The gate checks the shape and a clean reload on an unchanged tree; the corrupt-and-refuse path
belongs to a gate with its own copied module directory, which is the next slice. Under ASan the
first attempt leaked `module_dir`, which is exactly what the sanitizer is for.

Sixty-first: `ward_state_migrate`, the hole in the upgrade order. The host now binds the optional
export `ward_state_migrate(schema, ptr, len, out)` the ABI document froze but the host never
bound, calls it with a schema number and a blob in guest memory, and reads back a
`{ptr, len}` pair — the same descriptor dance `state_export` uses.

`tests/guest/echo.c` grew the export: it prefixes the incoming blob with the schema it came from
plus one, which is how the test proves the migration function actually ran rather than a
pass-through having happened. `tests/wasm_test` drives it end to end: export from one instance,
import into a second, migrate the same bytes into a third and check the prefix and the payload.

The binding is optional, like export and import, so no existing guest changes. The daemon does
not use it yet — that comes with pinning and `session.upgrade`, where an instance has a version
to move from.

Sixty-second: pinning. A session now records the module versions it started with: `wd_pin
{name, version}` on the session, and `wd_session_set_pins` appends a durable
`session.modules {modules: {driver: "3", provider: "1"}}` — a map, so a replay reads it back by
name rather than by position. `apply_pins` replays it beside `apply_model`, so a reloaded
session remembers what it was running.

The test gives pins their own session and asserts both directions: the event's sequence number
is real, and after closing and reopening the store the session carries the same two pins. It is
deliberately a separate session — adding an event to the existing one shifted every assertion
that counts sequence numbers, which is a small reminder that these tests encode their own
history.

Nothing reads the pins yet; resolution through them, and `session.upgrade`, are the next slices.

Sixty-third: moving the pin. `wd_session_upgrade` changes one pinned version and appends
`session.upgraded {module, from, to}`; upgrading to the version already pinned is a no-op that
appends nothing and answers no sequence, which is what makes a repeated `session.upgrade` free
rather than noise in the log. `apply_upgrade` replays the event beside `apply_pins`, so a
restarted session comes back on the version it moved to.

The test upgrades `driver` from 3 to 4, checks the pin and the sequence, then upgrades to 4
again and asserts nothing was appended, and finally reopens the store and finds `driver` at 4.

This is the session layer only. The daemon method — with the busy check that refuses an upgrade
while a run is live, and the CLI verb — is the next slice, and the instance swap behind it is
the one after that.

Sixty-fourth: session.upgrade over the protocol. The daemon serves it: it refuses while a run is
live (`busy`, the same check everything else uses), refuses a version the registry cannot
resolve (`not_found`), and otherwise moves the pin and answers the session object with
`upgraded: true`. Repeating the call answers `upgraded: false` and appends nothing.

That repeat exposed a real bug in the session layer: `wd_session_upgrade` only updated a pin
that already existed, so the first upgrade of a module the session had never pinned emitted the
event but never recorded the pin — and every later call emitted again. It now appends the pin
when it is new, which is what the replay path had been doing all along.

The gate drives both calls and asserts the difference.

Sixty-fifth: the handover. `wd_state_handover(from, to, from_schema, to_schema)` is the upgrade
order from the README in one function: export the state of the live instance, run
`ward_state_migrate` on the new instance when the schemas differ, import into it, and report
failure without touching the old instance if any step refuses.

The conformance test drives both paths: same schema is a plain export/import and the new
instance answers with the old value; a schema change runs the migrate and the new instance holds
the migrated bytes — the prefixed blob the echo guest produces, checked byte for byte.

This is the mechanism, not yet the daemon. Making a turn use it — resolve the pinned versions,
hand over to the new module, record `migration_failed` when it refuses — is the last step of the
M5 chain, and the one that turns the acceptance line from "proven with a fixture" into "the
session really moved".

Sixty-sixth: a trapping version is demoted. `wd_registry_trap` counts a trap per `(name,
version)` and marks the entry bad at two, which is the README's "repeated traps" — one bad input
should not condemn a module, and a fuel exhaustion caused by a pathological model output is
exactly the case that made a threshold necessary. `wd_registry_resolve` skips a bad entry, so a
third turn lands on the previous version instead. `wd_registry_clear` resets the count and the
flag, which is the administrative half.

The test takes a registry with two versions of the same module, traps v2 once and sees it still
resolve, traps it again and sees resolution fall back to v1, then clears it and sees v2 return.

Nothing calls `wd_registry_trap` from the turn path yet; that wiring belongs with the slice that
swaps instances, because that is where a trap is observable as a module's failure rather than a
caller's.

Sixty-seventh: the pins are visible. `wd_session_to_cbor` now carries a `pins` array of
`{name, version}` beside the id, cwd, model, created time and last sequence — built at map size
six, because this is the object the libcbor trap has already bitten twice and a size that is one
short silently drops the new key rather than failing.

The test asserts the shape through the object the protocol actually sends: two pins, the first
named `driver`.

This is a reporting change only. Nothing resolves through the pins yet; that is the join, and it
is still the one thing between this milestone and its acceptance line.

Sixty-eighth: a turn resolves through the pins. `wd_turn_new_pinned` takes the session's pins and
loads `<name>@<version>.wasm` when that file exists, falling back to `<name>.wasm` otherwise; the
daemon passes `session->pins` when it creates a run. `wd_turn_new` stays as the unpinned wrapper,
so the handful of tests that do not care are unchanged.

The test is the distinction that makes it a measurement rather than a shape: it copies the modules
to a temporary directory and corrupts `driver.wasm`, so an unpinned turn cannot be created at all,
then pins `driver@1` and asserts the turn is created from `driver@1.wasm`. That is resolution
happening, not a filename being passed through.

Still missing from the join: the state handover itself. The pins now choose the version; nothing
yet carries a module's exported state from the old version's instance to the new one, and
`migration_failed` has no path that writes it.

Sixty-ninth: a failed migration is durable. `wd_session_note_migration` appends
`session.migration_failed {module, from, to, reason}`, and replaying it moves the pin back to
`from` — which is the log expressing "keep the old instance", since the pin is what the next run
resolves through. A session whose handover refused therefore restarts on the version it was on
rather than on the one it failed to reach.

The test upgrades `driver` from 3 to 4, notes a failure moving 3 to 4, reopens the store and
finds `driver` back at 3. It also kept its own trap: the id was read from a session the store had
already closed, so the first run found an empty replay and the bug was in the test, not the
replay — copy the id before closing.

Nothing calls the writer yet. The caller belongs with the handover slice, which needs persisted
guest state to have something to hand over.

Seventieth: the design for the last piece of the join, written before the code because the ordering is
the whole difficulty. Guests live one turn, so nothing can be handed over unless the state of the
old version was *persisted* when its turn ended.

Where it lives: `<state>/<session>/modules/<name>@<version>.bin`. The version is in the filename, so
there is no sidecar to keep consistent, and a file's existence is the record that a module at that
version had exported state.

When it is written: after `turn.end`, the daemon asks each module for its export. A guest without
`ward_state_export` answers with an error, which means "no state" and is skipped silently; a guest
with one has its bytes written and the other versions' files for that name removed. Only the current
version keeps a blob, so a stale one can never be mistaken for live state.

When it is handed over: at `run_new`, before the turn is created, look for a blob whose version
differs from the pinned one. If there is none, nothing changes. If there is, the sequence is load
the old module, instantiate it, create the turn on the pinned version, call `wd_state_handover` from
the scratch instance into **the turn's** instance for that module, then delete the old blob and keep
the new one.

The ordering trap is the reason this is written down: `wd_turn_new_pinned` instantiates everything
at creation, so the handover must target the instance inside the turn rather than a second copy that
would then be thrown away. That means one new accessor, `wd_turn_instance(turn, name)`, and nothing
else in `turn.c`.

When it refuses: `wd_state_handover` fails having left the new instance untouched. The daemon then
calls `wd_session_note_migration` with `from` = the blob's version and keeps the run on that version,
so the turn must be created *after* the outcome is known — the resolve order is pins, stale blob,
decision, turn, handover. A run that falls back says so in the log and loses nothing.

The test is the one the milestone's acceptance line needs, and it needs a stateful pipeline module
first: the conformance guest from `tests/guest/echo.c` promoted to a pipeline member, or a small
`store`-shaped guest added under `modules/`. Until one exists, the handover is proven only between
two instances the test builds itself, which is what §65 delivers and what the parity note must say.

## Resuming M5

Where the milestone stands, so the next session does not have to reconstruct it: master is at
`c477b0a`, the suite is 28/28 in debug, ASan and UBSan, and nothing is uncommitted. Delivered:
the manifest loader, the registry (scan, hash, resolve by name and version), `module.list` and
`module.reload`, `ward_state_migrate` in the host with a conformance guest that proves it ran,
pinning (`session.modules`), moving a pin (`session.upgrade` over the protocol, with its busy
and `not_found` refusals), `wd_state_handover` (export, migrate, import, leaving the old instance
untouched on failure), bad-version demotion at two traps, and the pins reported on the session
object.

Left, in order:

1. **The join.** `run_new` should resolve each module through the session's pins, hand over
   instead of loading fresh when the version moved, and append `migration_failed` while keeping
   the old instance when the handover refuses. Both halves are tested; only the wire between
   them is missing. Until it lands, the milestone's acceptance line is "proven with a fixture",
   not "the session moved", and the note above says so.
2. **Trap reporting from the turn path**, where a trap is a module's failure rather than a
   caller's.
3. **`code.outline`, then the index** — nothing exists behind either, and `code.defs finds a
   symbol in the harness` is the acceptance line the README names for this milestone.

Two habits the last few rounds earned: assert the **no-op** case (a repeated upgrade that must
append nothing found a pin that was never recorded), and compare the live path with the replay
path rather than only checking that a prompt still works — the join is exactly that shape again.

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

Seventy-first: where a module's state is kept. `src/core/modstate.c` owns the layout the design
fixed: `<state>/<session>/modules/<name>@<version>.bin`, with the version in the filename so a
blob's existence dates it and no sidecar can disagree. It offers the path, write, read (where
"absent" is a distinct answer from "failed"), find-the-version-that-has-a-blob, and forget every
version except one.

Names are refused if they contain `/` or `..`, which is the same discipline the attachment store
uses; the state directory is not, because it is a path. The first version of the test failed on
exactly that — it validated the root as if it were a name.

`tests/modstate_test` measures all of it: write then read a blob back byte for byte, find the
version, forget the others and find nothing, refuse a traversal name, and forget everything.

Seventy-second: state leaves the turn. `wd_turn_state_export(turn, name, ...)` answers three things
that are not the same: zero with bytes, one for "this guest has no state", and minus one for a name
that is not a module of this turn. The distinction matters because the daemon must skip a stateless
guest silently and should complain about a typo.

When a run is freed and the turn still exists, the daemon asks each module for its state and, if
there is any, writes it to `<state>/<session>/modules/<name>@<version>.bin` — the version taken from
the session's pins — and forgets every other version of that module. Nothing is written for a
stateless guest, which is every shipped module today.

The test drives a real turn and asserts the stateless answer for `driver` and the unknown-name
answer, so the API's three outcomes are all measured even though no guest carries state yet.

The read side of the handover — finding a blob whose version differs from the pinned one and
handing over into the turn's instance — is the next slice, and it is now the only one missing
between a written blob and a moved session.

Seventy-third: the join is closed at the daemon. At `run_new`, after the turn is built on the
session's pins, `apply_state` looks for a stored blob for each module. Same version as the pin is
left alone — the blob is already valid. A different version is read, imported into the turn's own
instance, and the old file removed, because the next turn will export the new version's state
afresh. A refusal records `session.migration_failed`, which returns the pin to the old version, and
the turn is thrown away and rebuilt on that reverted pin: the run continues on the version that
worked rather than failing the prompt.

`wd_state_import_migrated` is the piece that made this small: the old instance is gone — its turn
ended — so the handover no longer starts from a live instance but from the blob the old one left.
Same schema imports directly; a different schema runs `ward_state_migrate` first. `wasm_test` drives
it with the conformance guest and checks the migrated bytes.

One limit is written down rather than hidden: the daemon passes the same schema in and out, because
manifests are per module and not per version. Real migration between versions needs per-version
manifests, and that is the next thing a stateful module will force.

Seventy-fourth: the code outline guest, first half. `modules/code` is pure and answers `{text}` with
`{symbols: [{name, kind, line}]}` — a scanner, not a parser: an identifier followed by a bracketed
argument list and then a brace, skipping the language keywords that look the same. It is labelled a
scanner everywhere because it is one, and tree-sitter is what would make it a real outline.

The first version found nothing: it only examined the *first* word of each line, so `int main(void)`
looked like the word `int` followed by a space. The rewrite walks every identifier and asks what
follows it, which is both simpler and right.

`tests/code_guest_test` measures two functions with an `if` inside them and a struct that must
produce nothing — the case that would pass if the scanner merely looked for braces.

Seventy-fifth: `/outline` reaches the scanner. Core grew the `code.outline` capability — it resolves
the path under the tool root with the same discipline as `fs.read`, reads at most 256 KiB, hands
the text to the `code` guest and formats its symbols as `name:line (kind)`. The skills guest asks
for it with `/outline <path>`, so the capability is granted to `skills` and the path is chosen by
the module while core only reads the file.

One bug, and it was mine in two places: the guest emitted `line` as a CBOR integer and core read it
with `wd_uint_of`, and the printed value was garbage — 2053 for a five-line file. Rather than chase
the decoder, the guest now emits the line as text and core copies it. That is the smaller surface:
a number that only ever travels to a formatter does not need to be a number.

`tool_test` writes a two-function C file, prompts `/outline sample.c`, and asserts both names with
their lines, so the whole path — skill, grant, capability, file read, guest, formatting — is
measured in one assertion pair.

Seventy-sixth: the symbol index. `src/core/index.c` walks a root — depth six, two hundred files,
256 KiB each, `.c` and `.h` only — hands each file's text to the `code` guest, and writes
`<cache>/symbols.tsv` as `relative path, name, line`, with an FNV-1a generation over those rows in
`<cache>/symbols.gen`. `wd_index_find` answers `path:line name` for an exact symbol name.

Core owns the walk, the cache and the generation; the guest still decides what a symbol is. That is
the split the README asks for, and it is why the same index will improve when tree-sitter replaces
the scanner without any core change.

Two of the test's own assertions were wrong before the code was: the search result had dropped the
symbol name because the query nulled the first tab before reading it, and the expected line for a
one-line header was `2` rather than `1`. Both were caught by the test disagreeing with itself,
which is the cheapest kind of catching there is.

`code.defs` — the method that searches the index by name and reports its generation — is the next
slice, and it is what the milestone's acceptance line names.

Seventy-seventh: `code.defs`. Core builds the index on first use into `<state>/cache/index`, then
answers `{ok, text, generation}` — `text` is `path:line name` per match, so the caller can act on it
without knowing the sidecar's shape. The skills guest answers `/defs <symbol>` through it, which
completes the row: outline the file, index the root, find the symbol.

The test prompts `/defs helper` against a written C file and asserts `sample.c:5 helper`.

One thing did not work, and it is worth recording rather than hiding: indexing this repository's own
`src/core` finds almost nothing, because the code guest's text buffer is 4 KiB and most core files
are larger — the guest truncates, and the truncated prefix often contains no complete definition.
A turn's file read is capped at 256 KiB and the index at 200 files, but the *guest* is the actual
limit. Raising its buffers is the next step for the index, and the acceptance line "finds a symbol
in the harness" cannot be claimed until it lands. The scanner also now crosses a wrapped signature
(`wd_turn *wd_turn_new(...)` split over two lines), which the earlier version missed.

Seventy-eighth: the code guest's buffers, and a limit that is still a wall. The text buffer went from
4 KiB to 16 KiB, the reply from 2 KiB to 4 KiB and the heap from 8 KiB to 64 KiB, which lets the
index read files up to 16 KiB instead of truncating most of them at 4.

Larger did not work, and the way it failed is worth recording: at 64 KiB of text and 8 KiB of reply,
`wd_instance_call` refused outright — instantiation succeeded, the call returned an error, and the
whole index came back empty. The cause is not yet identified, and it is not the index's own caps
(200 files, 256 KiB each). Until it is, 16 KiB is the working size and the acceptance line
"`code.defs` finds a symbol in the harness" stays unclaimed: a harness index does produce rows, but
not every symbol expected, and the discrepancy is unexplained rather than fixed.

The honest next step is to find that wall, not to raise a number and hope: bisect the guest's text
size against a single large file and read the host's error string, which no test prints today.

Seventy-ninth: the wall was mis-described, and the correction matters more than the size. §78 said a
larger text buffer made `wd_instance_call` fail; re-running it with the host's error string printed
shows the opposite — the call *succeeds* and the symbols come back empty, so the failure is
downstream of the call, in the reply the index decodes rather than in the call itself.

That distinction decides where to look next. A call error would point at the host's staging of input
and output; an empty reply points at the guest's encoder writing past what it can encode, or at the
JSON-free CBOR writer's bounds when `REPLY_SIZE` grows. The next attempt should print `reply_len` and
the first bytes of the reply at the 64 KiB size, which no test does today, instead of guessing at
buffer numbers.

The working size stays at 16 KiB text, 4 KiB reply and 64 KiB heap, which is what is committed, and
the harness acceptance line stays unclaimed.

Eightieth: the wall came down, and it was three things stacked. `code.defs` now finds a symbol in the
harness — `wd_turn_new` in `turn.c`, `wd_index_find` in `index.c` — asserted by the index test against
this repository's own `src/core`.

The three causes, each masked by the one before it:

1. The guest's text buffer was a *local* — a 64 KiB stack array on a WASI stack that is that size.
   Growing it trapped the call. It is `static` now.
2. With the buffer fixed, `turn.c` at 66 KiB still did not fit a 64 KiB limit, so the file was
   truncated before line 1145 where `wd_turn_new` is defined. The buffer is 128 KiB.
3. `SYMBOL_MAX` was 16, so even a whole `turn.c` could not list more than sixteen definitions and
   `wd_turn_new` was past the cut. It is 128, with a 16 KiB reply.

The earlier note in §79 that the call was succeeding was wrong: the instrument had been placed in a
test that did not make the failing call. The lesson is the one this repository keeps relearning —
put the probe where the failure happens, not where it is convenient.

Eighty-first: where M5 ends. Met and gated: module identity through the registry and manifests;
`module.list` and `module.reload`; `ward_state_migrate` in the host; pinning, moving a pin over the
protocol with its busy and not-found refusals; the handover between two instances; demotion after two
traps; modules resolved through a session's pins at turn creation; state exported when a run ends and
re-imported — migrating if needed — into the next turn, with `migration_failed` returning the pin and
the run continuing on the version that worked; `code.outline` end to end through the skills guest; the
symbol index with its generation; and `code.defs` finding a symbol in this repository's own source.

Not met: trap reporting from the turn path. `wd_registry_trap` exists and is tested on the registry, but
nothing in the turn calls it, so a version that traps at runtime is never demoted in practice. That is
the one row the milestone names that is still open, and closing it needs a module that traps on demand
plus the call-site wiring in `turn.c` and the daemon — a slice in its own right, not a footnote.

Also unmeasured, and said plainly: no shipped guest carries state across a turn, so "upgrade a stateful
module mid-session with no data loss" is proven between two instances and across a stored blob, but not
with a real stateful module in the pipeline. The mechanism is there and the fixture proves each half;
what is missing is a guest that needs it.

Eighty-second: trap reporting, so a bad version demotes itself in practice. A turn now records which
module trapped: `note_trap` runs wherever a module call fails and its error names a trap, an
unreachable instruction or exhausted fuel, and `wd_turn_trapped` answers the name. The daemon reads
that when a run is freed and calls `wd_registry_trap` with the session's pinned version.

The test uses a guest that does nothing but trap. It copies the module directory, puts the trap guest
where `driver.wasm` belongs, runs a prompt, and asserts two things: the turn comes back failed and
names `driver`, and after two recorded traps the registry no longer resolves `driver` at all — there
is no second version to fall back to in that fixture, which is exactly what "demoted" must mean.

That closes the last named row of the milestone at the level it can be closed without a stateful guest.
What still is not measured end to end is an upgrade of a module that carries state: every mechanism
exists and each half is tested, but no shipped guest keeps state across a turn, so the acceptance line
rests on a fixture and the notes say so.

Eighty-third: a user service. `contrib/openrc/ward.initd` and `ward.conf.d` follow the box's existing
precedent (`~/etc/init.d/openmessage`): OpenRC *user* mode, `supervise-daemon`, logs to `~/.dsh/logs`,
a pidfile under `XDG_RUNTIME_DIR`, and `start_pre`/`stop_post` that clear a stale socket only when no
live pid holds it.

Verified rather than written and left: installed into `~/etc`, started with `rc-service --user ward
start`, and the daemon came up and served a client over its socket. Two things only the run revealed —
`WARD_MODULE_DIR` must be exported or the daemon looks healthy while refusing every prompt with
"cannot read module", and the socket needs an explicit fallback because OpenRC sets no
`XDG_RUNTIME_DIR`. Both are now in the conf and in `docs/service.md`.

The sandbox mounts `~/.local/state` read-only, so the verification run pointed the service at
workspace paths; the installed configuration keeps the production paths, which is where the daemon
belongs when it is started from a real session.

Eighty-fourth: the runtime directory, when there is one. OpenRC sets no `XDG_RUNTIME_DIR`, so the init
script now chooses: if the directory exists and is writable it exports it and puts both the socket and
the pidfile there, giving `/run/user/<uid>/ward.sock`; otherwise it falls back to `/tmp/ward-<uid>.sock`
and a pidfile in `/tmp`. The fallback keeps the uid in the name, because a bare `/tmp/ward.sock` is the
one thing two users would fight over.

Verified on this box, where `/run/user/1000` exists: the service started, the socket appeared there,
`srw-------` as it should be, and a client listed sessions over it. The configuration no longer names
a socket path at all — it is commented as an override — so the script's choice is the only one.

Eighty-fifth: a bearer token, and the budget measured. The provider request had no authentication at all,
so `WARD_API_KEY` is now sent as `Authorization: Bearer ...` when set; the daemon also reads
`<state>/api_key` at startup — one line, trimmed — so the secret lives in the state directory at 0700
rather than in a world-readable conf. The test runs a real loopback server, captures the request, and
asserts the header is present with a key and absent without one.

The usage text was missing `session prompt`, which is the command that runs a turn; it is listed now.

And the honest arithmetic: `src/core` alone is 5,761 lines, but the budget in the README is for the
whole authority layer — core, proto, wasm, net and daemon — which is 13,749 once the CLI is excluded.
That is well past eight thousand and has been for a while. `src/daemon/server.c` alone is 3,330. This
is recorded rather than argued away: the next milestone's first work is either deletion or a stated
case for authority.

Eighty-sixth: two models, and the bugs a live one found. A model entry may now name an `endpoint`, and the
request names its model and bounds its length (`max_tokens`, 1024 by default) — the router will otherwise
generate until it feels like stopping, which for a 1.8B is 8191 tokens and 64 s of GPU. The bearer token
is only attached to non-loopback URLs, so a local router never receives the cloud key.

The live run found three faults that no unit test had:

- the request body was malformed — a stranded `",\"stream\":true` fragment after `"messages":[` — because a
  string edit left the old fragment in place. The provider test passed anyway, because it only asserted
  substrings. It now asserts the body has exactly one `"messages"` key and is brace-balanced.
- the session's model name was never put on the turn, so the router was asked for `"local"`.
- `max_tokens` was absent, so a small model streamed until it hit the context limit.

Verified end to end: `session resume <id> --from 1 --follow` after a prompt shows
`text.delta {"text":"Hello from Qwen! How can I assist you today?"}` — the local Qwen1.5-1.8B on llamb,
answering through ward with no cloud call.

Still open: `ward session prompt` and `ward -p` do not stream deltas; they return before the turn ends, so
the text is invisible unless the session is replayed. The daemon emits it correctly — this is the CLI's
subscription gap, and it is the next thing to fix.

Eighty-seventh: a remote endpoint, and both models answering. `net.request` refused anything that was not
loopback — a sane default from M1, when the only endpoint was a fake one in the tests. A model entry that
names its own endpoint is an operator decision, so that now sets `remote_ok` on the turn and the guard
admits the host. The bearer token still only travels to non-loopback URLs, which is exactly the pair that
matters for a cloud model: the key goes where the key belongs and nowhere else.

Verified with the key from `~/.dsh/credentials.yaml`, installed to `<state>/api_key` at 0600 (never in a
world-readable conf, never echoed into a log):

    session.prompted {"text":"Reply with exactly: hello from deepseek"}
    text.delta      {"text":"hello from deepseek"}
    turn.end

and the local rung, unchanged:

    text.delta      {"text":"Hello from Qwen! How can I assist you today?"}

Two models, two endpoints, one daemon: DeepSeek over TLS with a bearer token, Qwen1.5-1.8B on llamb with
no token at all.

The CLI still does not stream a turn it starts — `session prompt` returns before the turn ends, so the text
appears only on `session resume --from 1 --follow`. That is the next thing to fix, and it is now the only
thing between this setup and a usable one-liner.

Eighty-eighth: one key per model, not one key per daemon. A single `WARD_API_KEY` was wrong twice over: it
meant only one cloud provider could ever be authenticated, and it sent that one key to every remote host the
catalog named. `models.toml` now carries `key = "<name>"` — a credential *name*, never the secret — and the
daemon reads `<state>/credentials`, one `name=value` per line at 0600, resolving the name when it builds the
turn. The HTTP layer takes the key as an argument instead of reading the environment, so nothing is global
and nothing can leak between providers.

Verified after removing `api_key` entirely: DeepSeek still answers, which proves the lookup by name is what
authenticates it. `credentials_test` covers parsing, quoting, the comment and the malformed line.

Adding a provider is now two lines: `key = "openrouter"` in its model entry, and `openrouter=sk-…` in the
credentials file.

Eighty-ninth: a prompt assembly module, and why it is not wired yet. `src/core/prompt.c` builds the system
prompt the way DSH does — ordered sections, not one string:

- `identity` at -100, built in;
- any `<state>/prompt/NN-name.md`, the number is the order, so a user section slots anywhere;
- `rules` at 100 from `<cwd>/AGENTS.md`;
- `attachments` at 120 from the session.

A file with a built-in's name replaces it, so `100-rules.md` overrides the project rules and `-100-identity.md`
overrides the harness line. `wd_prompt_to_cbor` renders the assembly with each section's name, order, source
path and text — that is the transparency the design asks for, and `prompt_test` measures the ordering, the
override and the files that must be ignored.

It is not connected to a turn. Wiring it in leaked one entry per prompt under ASan (4521 bytes in 110
allocations over the test run) and I could not settle whether `cbor_array_push` increfs a borrowed item in
this libcbor build before running out of patience. The repository's own note says it does; the experiment said
otherwise twice. Rather than ship a leak, the module and its test are committed alone and the turn still uses
the old two-line rules loader. The next step is a two-file experiment that answers that refcount question
once, then the wiring is ten lines.

Ninetieth: the assembly reaches the model, and the refcount question is settled. libcbor's own header says it
plainly — "its reference count will be increased by one" on `cbor_array_push` — so a locally built entry is
pushed and then decref'd, while a borrowed one is pushed without an incref. Doing that in one place instead of
two is what the leak was: the previous merge kept an extra reference to every historical message.

Proven at the wire, not by inference: a request captured from a daemon whose `<state>/prompt/50-style.md` says
"Always answer in French" carries

    system -> You are an agent powered by ward: a small core that owns I/O
    system -> Always answer in French.
    user   -> [2026-11-02 00:46:00 EET] Say OK

in that order. The reply came back in English, which is the model ignoring the instruction, not the section
going missing — Qwen1.5-1.8B is not a reliable instruction-follower.

`prompt.preview {id}` and `ward prompt <id>` expose the same assembly without running a turn: each section's
name, order, source path and text, so an operator can see exactly what the model will be told and where each
piece came from.

Ninety-first: a session's own system prompt, set before it starts. `session.set_system {id, text}` appends a
durable `session.system {text}`, replay restores it, and the turn inserts it as the section named `session`
at order 5 — after the identity line, before any file section and before the project rules. A client can
therefore shape one session without touching the deployment's files, and the choice survives a restart
because it lives in the session log rather than in memory.

`ward session set-system <id> <text>` sets it and `ward prompt <id>` shows it in place, which is the
transparency the section design is for: the operator sees the identity line, the session's own text, the file
sections and the project rules in the order the model will receive them.

Verified: a session given "survive the restart" still shows that section after `rc-service --user ward restart`,
and the preview lists it between identity and the rest.

Ninety-second: a provider failure is now a diagnosis, not an empty answer. The provider reports the HTTP
status it got, and core classifies it: 501 `endpoint_unsupported`, 503 `model_limit`, 408/504
`provider_timeout`, 5xx or no status `provider_unavailable`, a 4xx whose body mentions the context
`context_exceeded`, any other 4xx `provider_rejected`. A transport failure is `provider_unavailable`.

On any of those the turn emits the frozen `error {code, name, message, turn}` and then
`turn.end {turn, aborted: true, reason}` — the shape the local-compat note asked for, and the shape that
was missing when a misconfigured endpoint produced silence. Compaction keeps its plain failure, because it
has no event sink and reports to its caller instead.

Verified against a dead port: the session replays `error {code:14, name:"provider_unavailable"}` followed by
`turn.end {aborted: true, reason:"provider_unavailable"}` instead of nothing at all.

The semantic change is the one the note flagged: a failed turn is `aborted` with an error event rather
than a normal end with no text. No version bump — adding an error name is compatible, and the old behaviour
was indistinguishable from success.

Ninety-third: the window comes from the server. `model.probe {id}` asks the endpoint for `/props`; a router
answers that with `role: router` and `n_ctx: 0`, so when the answer is useless the probe reads
`/v1/models`, finds the entry whose id matches the catalog's `model`, and takes `--ctx-size` and `--parallel`
out of its recorded argv. `context_source` says which happened: `server`, `config` (the `context` key parsed,
`32768` or `32k`) or `default` (8192).

Measured against the running router: `qwen-local n_ctx 8192 n_slots 1 from server`, which is exactly what
llama was started with — `--ctx-size 8192 --parallel 1`. The number in `models.toml` happens to agree here,
which is why the source matters: on the next rung it will not.

The catalog also grew `max_output`, `device`, `slots` and `stop`, and `model.list` serves them, so a client
stops inferring capability from a model's name. `probe_test` covers the key scan, the router-argv scan, the
`32k` parsing and the absent-model case.

Still to do for §3: the budget arithmetic that consumes `n_ctx` — the 8192-byte prompt budget in the daemon
is the number this probe exists to replace.

Ninety-fourth: the budget follows the window. `wd_context_budget(n_ctx, max_output)` is the arithmetic the
note asked the module to own: reserve the output tokens plus 256 of headroom, turn the rest into bytes at four
per token, clamp to [4 KiB, 1 MiB]. `history_budget()` asks the model's probe for `n_ctx` and applies it, and
the three places that built history with a literal 8192 now use it — the prompt path, the parent's history and
a subagent's.

For qwen-local that is 27 KiB of history instead of 8; for deepseek, 258 KiB. The clamp keeps a tiny window
honest rather than letting the arithmetic go negative.

Not yet measured end to end: the effect is that more history survives trimming, which needs a session with
tens of kilobytes of turns and a capture of the request body to observe. The arithmetic is tested; the wiring
is three lines; the observation is owed.

Ninety-fifth: the head can now be byte-stable, and the reason it was not. The driver no longer stamps the
prompt with the wall clock — that stamp lived only in the outbound request while `session.prompted` recorded
the raw text, so replay rebuilt different bytes and llama.cpp re-prefilled from the first user turn every
time. The daemon now appends a durable `session.time {text}` before each `session.prompted`, and replay turns
that event into a `system` message, so the bytes the model saw are the bytes the log holds.

`session_test` asserts the property that matters rather than a message count: history after one turn is a
byte-prefix of history after two, compared with `cbor_serialize_alloc` per entry. Counting messages would not
catch a moved byte, which is exactly how the stamp survived this long.

The first attempt at the broadcast was a use-after-free: `wd_session_emit` consumes its payload, and the same
map was then handed to `wd_event` for the broadcast. ASan caught it in `gate_crash` and `gate_tool` while the
debug build passed, which is the sanitizer earning its place. The event map is now built separately.

Ninety-sixth: history is no longer trimmed silently. `wd_session_history` keeps every turn; the budget
parameter stays for callers but no longer drops from the front. The daemon measures the assembled history
against the probed budget and, when it is larger, appends a durable `session.overflow {bytes, budget}` before
the prompt — the drop becomes a visible event rather than a quiet subtraction, and the provider's own
`context_exceeded` is what a client acts on.

The test now asserts the opposite of what it used to: a tiny budget returns the whole history. That assertion
is the guarantee; the overflow notice itself is not yet observed end to end, because producing a history
larger than 4 KiB in the fixture needs eleven turns and the loop that drives them was still racing the busy
check when this was written.

Worth recording from that attempt: a 6 KB user prompt does not overflow anything, it fails at the driver's
512-byte text buffer and surfaces as a bare "turn failed". The error taxonomy covers provider failures only,
so a local failure like that one is still silent. That is a gap in §6's reach, not in §2.

Ninety-seventh: the head is frozen. On a session's first run the daemon assembles the sections once, renders
them into one text with blank lines between, and appends a durable `session.head {text}`; every later turn
sends that stored text as the single leading system message instead of re-reading `AGENTS.md` and rebuilding.

`session.set_system` does not silently invalidate it — a session whose system text changes keeps its old head
until something explicit resets it, which is deliberate: rewriting the head mid-session is exactly the
re-prefill this work exists to avoid. Changing a frozen head wants a branch, and that is the same decision the
note asks for around compaction.

Measured: two prompts in one session now emit exactly one `session.head`, and the gate's rules assertion still
passes because the head is built from the same sections that carried `[rules]` before.

A wrong turn worth recording: the first attempt put the freeze in a spot that compiled but never ran, so the
gate failed on `[rules]` and the live session had no head at all. The test caught it in one run; the fix was
to place the block in `run_new` beside the model and system wiring, and to re-apply the head in the rebuild
branch too.

Ninety-eighth: compaction branches. `session.compact` no longer rewrites the session it was asked about: it
forks the log at the current sequence, records `session.compacted` on the **child**, and answers
`{session, parent, branched, through, summary}`. The parent keeps its transcript and its llama.cpp cache, which
is the point — compaction used to discard every turn in place and force a full re-prefill. `in_place: true`
keeps the old shape for callers that want it.

`session_test` asserts both halves: the branch's history is a single system message, and the parent still has
all six entries afterwards.

`ward session compact <id>` drives it, and the live attempt failed with `provider_rejected` — the compact
request came back non-200 from the local router while the router's own log shows a 14-token completion, so
something in the compact request shape is off. That is a real bug, surfaced rather than hidden by the new error
taxonomy, and it is the next thing to chase: the branch machinery and the summary call are separable, and the
summary call is the part failing.

Ninety-ninth: three bugs under one symptom, found by refusing to guess. `session.compact` returned
`provider_rejected` and then `no summary`; each was a separate fault, and each hid the next.

1. The compaction request never named the model — it goes through `provider_phase` directly rather than the
   turn's merged payload — so llama answered `model 'local' not found` with a 400. That is why §6's taxonomy
   was worth building: the body said so, once the code printed it under `WARD_DEBUG`.
2. The provider's `body` buffer was 16 KiB and a real SSE completion is 50 KiB of chunked JSON. `wd_r_text`
   refused it, the guest returned *without writing a reply*, and the host read the guest's previous reply
   buffer — the phase-one request. That is why a decode seemed to succeed and produce a request instead of a
   summary. The fix is a 64 KiB buffer *and* the lesson: a guest that cannot parse its input must write an
   error reply, never return silently.
3. Raising that buffer put three 64 KiB arrays on the guest's 64 KiB WASI stack, which trapped in `ward_call`.
   They are static now — the same mistake the `code` guest made, which is why it is now written down twice.

With those fixed, compaction branches and returns a summary, and `ward session compact` prints both session
ids. The summary itself is a 1.8B model's invention, which is a model problem, not a plumbing one.

Hundredth: cancelling stops the helper, and the router's timings are recorded. `wd_http_job_cancel` sends
SIGTERM, waits up to 200 ms for the child, then SIGKILL and reaps; `wd_turn_abort` uses it before freeing the
job, so `session.cancel` no longer sits in `waitpid` for the request timeout. `http_cancel_test` starts a job
against a listener that never answers, cancels it, and asserts the whole thing finishes inside two seconds —
measured 0.01 s against a 60 s timeout.

The provider now reads llama.cpp's `timings` out of the completion body and core appends a durable
`turn.timings {prompt_n, prompt_ms, predicted_n, predicted_ms, cache_n}`. Measured on a real turn:
`{"prompt_n":257,"prompt_ms":10,"predicted_n":3,"predicted_ms":11,"cache_n":44}` — which is the evidence a
prefix-cache is actually being reused (cache_n > 0) and the number that makes a 40 s prefill diagnosable.

Still open in §5: the response is still read whole before a single `text.delta` is emitted. The buffer work in
§6's fix made that safe (64 KiB body, static arrays), not streamed.

Hundred-and-first: the router's own tokens are now the stats. `wd_session_stats` sums `turn.timings` over the
log — prompt, predicted and cache tokens, and both millisecond figures — and `stats.session` reports them
beside the byte counts. `tokens_estimated` stays true only when no timings were recorded, so a local model's
numbers are measured and a cloud model's remain a labelled estimate until its provider returns the same fields.

That is the last piece of §5 short of true streaming, and it closes the loop on §2: the `cache_n` a turn records
is the evidence that a frozen head is being reused rather than re-prefilled.

The map-size trap bit again, in the same place it always does: adding three keys to a `wd_map` built for seven
silently dropped `cost_total`, and the gate caught it with `cbor_isa_float_ctrl`. Ten keys, thirteen when
priced — counted rather than guessed.

Hundred-and-second: the wait has a name. A turn now emits `turn.state` as it moves through the provider:
`prefilling` when the request is parked, `answering` when the reply is being read, `idle` when it is done. On a
real turn the log reads 5 prefilling, 6 answering, 10 idle — three durable events where before there was
silence for the length of a prefill.

That is the note's "make the 40 s wait visible" without touching how text is delivered, which is still one delta
at the end. True streaming stays open; this makes the stall attributable while it happens rather than afterwards
in `turn.timings`.

The investigation also found something about the harness rather than the product: leaked `ward up` daemons from
earlier gate runs were still holding sockets and state directories, and a stale one made a live turn look wedged.
The suite leaves daemons behind when a gate fails part-way. That is a test-lifecycle bug and the next thing to
fix, because a leak that makes the *product* look broken is worse than the failure that caused it.

Hundred-and-third: a gate's daemon now dies with the gate. Each gate sets `PR_SET_PDEATHSIG` in the child
before exec, so a gate killed by an assertion, a timeout or a signal cannot leave a `ward up` holding a socket
and a state directory. The kernel does the cleanup; no atexit handler can be skipped by `abort()`, which is
exactly how the leak survived.

Verified the way the leak was found: start `gate_tool`, abort it mid-run with SIGABRT, and count — three daemons
before, one after, and the survivor is the user service. The suite leaves nothing behind in debug, ASan or
UBSan.

That is worth as much as a feature. Five stale daemons made a live turn look wedged for several minutes, and the
wrong hypothesis (a stuck provider) was only abandoned because `ps` disagreed with it. A test that fails must
leave the machine as it found it, or it costs the next person the same minutes.

Hundred-and-fourth: the queue is FIFO and says where you are. `enqueue_turn` appended at the head, so a queued
background turn ran last — the note flagged it and it was a two-line fix. `job.list` now also reports `ahead` per
entry: zero for a running job, its position for a queued one, which is the number a client needs to explain why
a subagent has not started.

This is the honest half of §7. The device semaphore — a pool keyed by the model's `device` with `slots` from the
catalog, acquired before the provider call and released on finish, so two subagents on one card serialise
instead of thrashing — is still ahead, and it needs care with the run pool and the queue that this change just
made orderable.

Hundred-and-fifth: a device holds its slots. A model entry that names a `device` and `slots` puts its runs through a
per-device pool: a run acquires a slot when its turn is created, releases it when the run is freed, and a prompt
whose device is full is queued instead of started. The queue drains only when the head's device has room, so a
full card serialises its work rather than pre-empting it, and `job.list` answers a `devices` array with each
pool's name, slots and inflight count.

Verified against a one-slot device on the fixture model: with two sessions prompting back to back, the first
reads `running` and the second `queued`, and the second runs when the first finishes. Without a `device` key
nothing changes — which is why the existing subagent concurrency gate still passes.

Two honesty notes: the pool is per daemon and per process, so a second daemon with the same card is invisible to
it, and the queue holds at most what it holds — there is no per-session cap yet, which the note asks for so one
parent cannot starve another.

Hundred-and-sixth: one queued turn per session. `enqueue_turn` refuses a second item for a session already waiting,
and the prompt path answers `busy: the session is already queued` rather than starting a second run behind the
first — which would have run twice when the device drained, because the queued item and the immediate run were
the same turn. The note asked for the cap so one parent cannot starve another; the starvation it actually
prevents is duplicating the parent's own work.

Tests pass in all three builds unchanged: the existing queue assertions already bound the count.

Hundred-and-seventh: the provider can see a native tool call. When the completion carries `tool_calls`, the
provider pulls out the first `function.name` and its `arguments` string and answers `{tool, arguments}` beside the
text. That is the parse half of §4: the guest now reports what the model asked for rather than swallowing it as
text.

The test feeds a real tool_calls-shaped SSE body and asserts the name and the argument text survive the escape
handling. Its first placement put the block after `wd_instance_free`, which ASan reported as a use-after-free in
`wd_instance_call` — the test was wrong, not the guest, and moving the block before teardown fixed it.

Still ahead for §4: dispatch. `process_action` must turn a `tool` reply into an `invoke` so the driver's tool path
runs, the grammar-only fallback, the lenient parse with one repair, and refusing multiple calls instead of
silently taking the first.

Hundred-and-eighth: the design for tool dispatch, written down before the code because the shape matters more
than the lines. The provider already reports `{tool, arguments, calls}` when a completion carries `tool_calls`; the
loop that consumes it is three small pieces.

1. In `wd_turn_resume_http`, after `provider_phase`, a reply with a non-empty `tool` is not handed to the driver as
   `{kind:"result"}`. It becomes `{kind:"tool", tool, arguments}` — a shape the driver can recognise without
   parsing the provider's reply struct.
2. The driver gains that branch beside its slash-command ones: it scans the arguments text for `"path"` and
   `"text"` (a substring scan, not a JSON parser — the guest has no parser and the note's own advice is to keep
   schemas flat), then calls the existing `invoke_tool` for `read`, `write`, `grep` or `edit`. An unknown tool or
   missing field answers with a text delta naming the problem rather than inventing a call.
3. The tool result returns through the ordinary `invoke` path, so `perform_capability`, approvals and the worker
   are unchanged — which is the point of routing through the driver rather than dispatching from core.

The multi-call refusal belongs in (1): a reply with `calls > 1` ends the turn with `tool_parse` instead of taking
the first. The first attempt at counting calls produced a garbage field and was reverted; the cause was not found,
so the next attempt should count in the *provider* over the raw body it already scans, and test that count in
`provider_test` before any core change.

Hundred-and-ninth: a multi-call reply is refused, not truncated. When the completion carries more than one
`function` entry, the provider answers with an empty `tool` and a text naming the refusal, so the driver has nothing
to dispatch and the client sees why. That is the note's rule — multiple calls are a parse failure, never "run the
first and forget the rest" — and it needed no new reply field, which is where the earlier attempt went wrong: the
extra key decoded as garbage and was reverted rather than shipped.

The test feeds a two-call body and asserts an empty tool plus the refusal text; `provider_test` is the right place
for it because the count lives in the provider, over the body it already scans.

Hundred-and-tenth: a tool call now reaches the driver. When the provider reply carries a non-empty `tool`,
`wd_turn_resume_http` stops handing the driver `{kind:"result"}` and sends `{kind:"tool", payload:{tool, arguments}}`
instead. The driver gains that branch beside its slash commands: it scans the arguments text for `path` and `text`
with a small `json_field`, checks policy exactly as `/read` and `/write` do, and calls the existing `invoke_tool`.
An unknown tool, or one with no name, answers with a text delta naming the problem rather than inventing a call.

The result returns through the ordinary `invoke` path, so `perform_capability`, approvals and the worker are
untouched — the reason to route through the driver instead of dispatching from core.

What is *not* done, stated plainly: no test exercises this branch end to end. The reply keys only appear when a
model actually returns `tool_calls`, and the gate fixtures serve canned completions only to `provider_test`. The
branch is unreachable from every existing path — which is why three suites stay green unchanged — but a feature
whose new path nothing runs is a claim, not a measurement. The gate it needs: a fixture endpoint that answers the
first completion with the two-call-free `tool_calls` body `provider_test` already uses, then asserts a `tool.call`
event and a `tool.result` for a `read` of a file written by the fixture, followed by a second completion carrying
ordinary text so the turn ends.

Hundred-and-eleventh: the tool-dispatch gate was attempted and abandoned for this session. The fake endpoint grew a
branch that answers a prompt containing "use a tool now" with a single `tool_calls` reply, and the gate watched the
event stream for a `tool.` kind before `turn.end`. It hung instead of asserting: either the turn never ended (the
driver's continuation after a tool result asks the provider again, and my "not already answered" guard keyed on the
absence of an assistant role in the request, which the second request no longer satisfies) or the awaited event
never arrives and a blocking reader waits forever.

Reverted rather than left in the tree, because a gate that hangs is worse than a documented gap: it costs a timeout
in every future run and teaches the next person to distrust the suite. The dispatch code from the previous entry
stays, still unreached by any test, and the recipe in that entry stands. A gate for it wants a fake endpoint that
counts requests rather than sniffing their text, so the first request answers with tool_calls and every later one
with ordinary text.

Hundred-and-twelfth: the tool-dispatch gate passes. Two changes made the difference, and both were the fixes the
abandoned attempt named.

The fixture counts marker requests in a file instead of sniffing the request text: the first prompt containing "use
a tool now" is answered with a single `tool_calls` reply, every later one with ordinary text, so the loop that hung
the gate cannot happen. The gate then reads events under a 15-second `SO_RCVTIMEO` rather than waiting forever, and
asserts it saw a `tool.` event before the turn ended.

Measured: `gate_tool` passes in all three builds with that assertion in place, which means the chain from a
provider reply carrying `tool`, through `{kind:"tool"}`, through the driver's `json_field` scan, to an actual
`invoke` of the tools module, is now exercised rather than assumed. The dispatch landed in `b305ea9` untested; this
closes that gap two entries later.

Hundred-and-thirteenth: a tool call written in the text is still a tool call. When a completion carries no native
`tool_calls`, the provider strips the SSE's backslash escapes from the answer and looks for a `{"tool":…,"path":…}`
object; finding one, it answers with the same `{tool, arguments}` shape the native path produces, so the driver
needs no second protocol. The arguments are rebuilt as `{"path":"…"}` so the driver's existing `json_field` scan
works unchanged.

That is the note's "lenient parse" for the common failure of a model that writes the call instead of emitting it.
The gate is in `provider_test`: a completion whose content is an escaped JSON call yields `tool == read` and an
arguments string naming the path.

The one-repair step is still open: this accepts a call the model wrote in prose, it does not yet re-ask once when
the parse fails, and a `tool` without a `path` is treated as no call at all.

Hundred-and-fourteenth: a malformed written call is said out loud. When the text holds `{"tool":"read"}` with no
path, the provider now answers with an empty tool and the text "the model asked for a tool without a path; refusing
rather than guessing", where before the call would have been dispatched with an empty path and failed somewhere
deeper. Same shape as the multi-call refusal, and the test asserts both halves.

It is the same principle as the error taxonomy: a failure a client can read is worth more than a failure buried two
layers down. Still open is the one *repair* — re-asking the model once when the parse fails — which needs a second
provider phase rather than a better parse.

Hundred-and-fifteenth: the streaming plan, with the anchors found rather than guessed. The helper already frames its
answer: `wd_http_start` (src/net/http.c:133) forks curl, and `wd_http_job_finish` (:200) reads an eight-byte header
— four bytes of status, four of length — then exactly that many body bytes. So the wire between the helper and the
daemon is a length-prefixed message, which is why a whole completion arrives at once.

The five edits, in order, each of which keeps the tree green on its own:

1. `write_cb` (src/net/http.c:21) writes each chunk to the pipe as it arrives instead of appending to a buffer, and
   the child writes the four status bytes from a `HEADERFUNCTION` before the first chunk. Length is not known in
   advance, so the frame becomes "status, then bytes until EOF"; the parent learns completion from EOF instead of a
   length.
2. `wd_http_job_finish` reads the status, then everything to EOF, so the daemon still gets the full body and every
   existing path behaves exactly as before. This step alone is invisible and safe; it is the one that proves the
   framing change without touching the daemon.
3. `run_watch` for the http fd (src/daemon/server.c:2195) gains an incremental read: append to a per-run buffer,
   leave the fd registered, and call `http_done` only on EOF.
4. On each read, feed the new bytes to a small SSE scanner that emits a `text.delta` per complete `content` fragment,
   carrying a partial UTF-8 sequence and a partial `data:` line across reads. `[DONE]` closes the stream early.
5. The driver must not repeat the text: core marks the turn as streamed and sends `{kind:"result", payload:{text:""}}`
   so the driver's own delta is empty, or the provider reply gains a `streamed` flag the driver honours.

Step 4 is the one a gate can measure: with `WARD_FAKE_DELAY_MS` the fixture already spaces its two content chunks by
seconds, so a turn test can assert that the first `text.delta` arrives before `turn.end` rather than after it.

Hundred-and-sixteenth: the helper streams. Steps one and two of the plan are in: `wd_http_start`'s child now runs
curl itself with a write callback that pipes each chunk straight through, sends the status from a header callback
before the first chunk, and the parent reads status then everything to EOF instead of a declared length. Nothing
downstream changed, which is the point — every existing path sees the same `wd_http_response`.

Proven the useful way: a live turn failed with the router refusing to load the model, and the *whole* JSON error body
arrived intact through the streamed path (`provider_unavailable`, message `model ... failed to load`). Before this
the same failure would have travelled as a length-prefixed blob; now it travels as a stream, and §6's taxonomy turned
it into a named error the client can read.

The router itself is currently unable to load `qwen1_5-1_8b-chat-q4_k_m` — `Kev-0.8B` and the embedding model hold
most of the 2 GiB card, so a direct curl fails the same way. That is the box, not ward, and no test depends on a live
router.

Next: step three, reading the pipe incrementally under epoll and emitting deltas before EOF.

Hundred-and-seventeenth: the fourth step of the streaming plan is mostly already built. `src/proto/sse.h` carries a
complete incremental SSE parser — `wd_sse_create`/`wd_sse_feed`/`wd_sse_finish` with an event callback, line and
data size limits, and escape handling — written for the QUIC and event paths. My first attempt at step four added a
second parser in `src/core/stream.c`; the linker caught it as `multiple definition of wd_sse_feed`, which is the
best possible outcome for a duplicated abstraction.

Deleted, with the Makefile line. Step four is therefore a *user* of `wd_sse_*`, not a new parser: feed each pipe
read into the existing instance, take `content` out of the event data, and emit a delta per event. That also means
the partial-UTF-8 concern is bounded — the parser emits whole events, so a fragment is never split mid-character
beyond what the provider already sent.

A reminder worth keeping: check what the tree already has before writing the abstraction it needs. `src/proto/sse.h`
has been there since M1 and the plan I wrote two entries ago did not mention it — I had read `http.c` and not the
neighbouring protocol directory.

Hundred-and-eighteenth: the daemon reads the provider stream as it arrives. `http_done` no longer waits for the
whole answer: it appends what the pipe offers to a per-run buffer, returns when the read would block, and only on
EOF parses the four status bytes and hands the body to `wd_turn_resume_http_body`. The job path still works through
`wd_turn_resume_http`, which now finishes the job and delegates, so there is one resume implementation instead of
two.

The buffer grows by doubling from 128 KiB, is freed in `run_free`, and the suites pass in all three builds — the
provider path is covered by the subagent and tool gates, so a mistake here would have been loud.

What this buys immediately is bounded memory per read rather than per answer, and what it enables is step four: the
same buffer is where the SSE parser will look for content as it fills. The deltas are still not emitted; that is the
next edit, and it is the one a gate can finally measure.

Hundred-and-nineteenth: deltas stream. Each pipe read is fed to a per-run `wd_sse` instance whose callback pulls `content`
out of the event data and appends a durable `text.delta` immediately, so a turn's text now arrives while the provider
is still generating. The driver's own single delta is suppressed when the run has already streamed at least one, which
keeps the transcript from repeating the answer and lets history rebuild from the streamed pieces alone.

That completes the five-step plan: helper streams, parent reads to EOF in a growing buffer, the daemon parses
incrementally, and the driver stays quiet. All three suites pass — the subagent, tool and crash gates all go through
the provider path, so an ordering mistake would have broken them.

Honest caveat: the *timing* assertion the plan promised — first `text.delta` before `turn.end` under
`WARD_FAKE_DELAY_MS` — is still not written. The behaviour is implemented and nothing regressed, but "delta arrives
early" is asserted by no test yet; the next slice is that assertion, and it either passes quickly or shows the
suppression guard firing where it should not.

Hundred-and-twentieth: the streaming timing assertion was attempted and reverted. Adding `assert(turn_deltas >= 2)` to the
existing `turn_text` call hung `gate_tool` rather than failing it; removing the two lines made the same gate pass again
immediately, twice. Why an assertion can hang a gate is not obvious — a failed assert aborts — so the honest reading is
that either the counter disturbed the event accounting or the gate has an intermittent hang that my change merely made
more likely. The evidence does not separate those.

Left out of the tree, because a gate that hangs is worse than a gate that is merely missing, and because I cannot yet say
which of the two causes it is. Next attempt should instrument the gate: print each consumed event kind with a sequence
number so a hang shows the last event it saw, then add the delta assertion behind that.

Hundred-and-twenty-first: the hang was an artifact and the real result is a number. Running the gate with its output
redirected to a file rather than a pipe made it fail in seconds instead of appearing to hang — the assertion aborted,
an orphaned fixture child kept the pipe open, and `tail` waited for an EOF that never came. So the earlier "hang" was
my own command's plumbing, and the gate itself was fine.

The measurement: `deltas=1` for a completion the fixture sends as two `data:` chunks. So the streamed path produced one
delta where two were expected — either the parser merged them into a single event, one line was dropped, or the fixture's
first chunk was empty for that prompt and emitted nothing. Which of those it is needs one more line of instrumentation:
count the events the SSE callback sees, not just the deltas it emits.

Recorded, reverted, and left green. The streaming behaviour itself is unchanged and the suites pass; what is missing is
still the assertion, and now it is missing for a known reason rather than an unknown one.

Hundred-and-twenty-second: counting the events settled part of it. With `WARD_DEBUG` printing each SSE event it sees, the
gate's four provider replies produced **eight** events — two per reply: one content event and one six-byte `[DONE]`. So
the parser is not merging or dropping anything, and the daemon emits exactly one content delta per reply because the
fixture sends exactly one non-empty content chunk per reply. The `deltas=1` measurement was correct and the assertion
was wrong: `strlen(prompt)/2` is zero for a one-character prompt, so the first `%.*s` chunk is empty and carries no
content.

That means streaming works and the gate I wanted does not need a bigger constant — it needs a fixture prompt long
enough that both halves are non-empty, or an assertion on the *event* count rather than the delta count. The debug print
stays: it costs nothing outside `WARD_DEBUG` and answers this exact question in one run.

Hundred-and-twenty-third: a longer prompt did not produce a second content event either. The gate's log shows two provider
replies on the SSE path with one content event each (57 and 91 bytes) plus `[DONE]` — so the fixture's second `data:` line
never reaches the wire as an event, whatever the prompt length. Either the second `%.*s`/`%s` chunk is empty for the text
the fixture extracts, or its two lines are being written as one.

That means the assertion cannot be written against the fixture as it stands, and the fixture is the thing to fix: it should
emit two obviously non-empty fragments (fixed strings, not halves of a request field) so the split is controlled by the
test rather than by the length of whatever the request happened to carry. Reverted again rather than leaving a failing
gate; the fix belongs in `tests/fake_endpoint.h`, which is where the next attempt should start.

Hundred-and-twenty-fourth: the missing deltas were the status frame. The daemon fed every byte it read into the SSE
parser, including the four-byte status prefix, so the first `data:` line arrived as `\0\0\0\0data: {...}` and was
dropped; only the second fragment survived, which is exactly the one-delta-per-reply the log kept showing. Tracking
whether the status has been consumed and skipping those four bytes fixed it: each reply now yields two content events
plus `[DONE]`.

With that, the timing gate is writable and written: `turn_text` counts the deltas it sees, and the gate asserts at
least two for a single prompt — which can only pass if the daemon emitted fragments individually rather than after the
whole answer. `gate_tool` passes in all three builds.

Four rounds, three wrong hypotheses, and the cause was two layers below where I was looking: not the fixture, not the
parser, but the four bytes in front of the body. The fixture hypothesis was partly right, though — the first attempt
used a prompt whose first half was empty, which hid the real fault behind a plausible one.

Hundred-and-twenty-fifth: a device slot that other processes can see. `wd_device_acquire` takes an exclusive `flock` on
`<state>/device-<name>-<slot>.lock`, trying each slot in turn, so two daemons sharing one card serialise instead of each
believing it has the only slot. `wd_device_release` unlocks and closes; `devlock_test` holds one slot, forks, and asserts
the child is refused with one slot and admitted with two.

That closes the first of §7's two documented limits at the library level. Wiring it into the daemon is the next step: the
in-process counter stays authoritative for queueing, and the file lock is consulted when acquiring, releasing on
`run_free` — with the busy case feeding the same `enqueue_turn` path a full pool already uses. Not wired yet, and the
test says nothing about the daemon until it is.

Hundred-and-twenty-sixth: the device lock is in the daemon's path. `run_new` acquires the file lock with the run's
slot count and refuses with "the device has no free slot" when another process holds them; `run_free` releases it;
`device_free` probes by taking and releasing a slot, so the queue decides on evidence from both the in-process counter
and the filesystem. The probe is what makes a second daemon visible.

The banked state had one bug worth naming: the acquire-failure path began with `srv->runs = run->next;`, copied from a
teardown that runs *after* the run is linked. At that point the run is not yet on the list, so the line would have set
`srv->runs` to NULL and dropped every active run. Removed before committing; both sanitizer builds pass.

Still unmeasured: two daemons actually contending for one device. The lock test proves the mechanism at library level
and the daemon now uses it, but nothing yet starts a second daemon against the same state directory and asserts the
second turn waits.

Hundred-and-twenty-seventh: one repair. When the provider reports a tool with empty arguments — a call the model wrote
without its path — core now asks once more, with a strict instruction and the model's own previous text appended, and
uses that answer if it carries a usable call. `repair_tried` bounds it to one attempt; a second failure passes the
provider's refusal through and the turn ends normally.

Two bugs surfaced on the way, both in the daemon's provider watching. `run_watch` returned early when the run's recorded
fd equalled the new job's fd, which happens constantly because the old child's fd number is reused — but closing the old
fd had already removed it from epoll, so the early return left the new fd unwatched and the repair request's answer was
never read. That early return is gone; the watch is always re-armed. And the per-run stream state is reset when a repair
request starts, so the second body is not appended to the first.

What is not proven: the gate for the repair path. Tracing it showed the repair request submitted and its answer parsed as
`tool=read`, then `judge.decision` and `turn.end` with no `tool.call` — so the dispatch after a repair is being stopped
somewhere the ordinary path is not, and the gate assertion failed on that. The code is in, the ordinary tool gate still
passes, and the repair path is measured only up to "the second call is parsed".

Hundred-and-twenty-eighth: ward has its own cgroup, builtin. `wd_cgroup_enter` creates `/ward` and `/ward/harness`
recursively, enables the memory and pids controllers at every level it creates, writes 2 GiB to `memory.high` and
`memory.max`, zero to `memory.swap.max` and 256 to `pids.max`, then moves its own pid into the leaf. It checks two
things first: that the fence holds no processes (a cgroup with children may not hold processes) and that the
supervisor is not inside the fence — the OpenRC `supervise-daemon` stays in its session cgroup, which is what keeps
an OOM inside the limit from taking the supervisor with it. `wd_cgroup_leave` moves back and removes the leaf on a
clean shutdown.

The interesting constraint is cgroup v2's migration rule: a process may only be moved if the writer can write the
`cgroup.procs` of the closest common ancestor of both cgroups. A user service starts in `/dsh/svc/session-…`, whose
common ancestor with `/ward` is the root cgroup, owned by root — so a plain write is refused with EACCES however the
leaf is chowned. ward therefore writes directly when it can and otherwise asks root for that single write
(`sudo -n tee`), with `WARD_CGROUP_SUDO=0` to forbid it.

Verified on the running service: the daemon's cgroup is `/ward/harness`, `memory.max = memory.high = 2 GiB`,
swap 0, pids 256, and `supervise-daemon` is in `/dsh/svc/session-…`, outside `/ward`.

Tests live under `/ward/test/gate-<pid>`: each gate gets its own fence and leaf, removes it on exit, and sweeps
leaves left by a gate that was killed. `cgroup_test` enters a temporary fence and checks the limits the kernel
actually recorded.

Hundred-and-twenty-ninth: the protocol's tool events existed only on paper. `tool.call` and `tool.result` are in
`README.md`'s frozen event list and in `docs/protocol.md`, and nothing emitted them — a grep for either name across
`src/` and `modules/` returned nothing. The invoke path now emits `tool.call {tool, call, module}` before the
capability runs and `tool.result {tool, call}` after it returns, both carrying the *capability* name (`fs.write`, not
the driver's action name), and `gate_tool` asserts both events on an approved write between `approval.resolved` and
the first delta.

That also explains something I had recorded wrongly: the earlier "tool-dispatch gate" that passed in round
twenty-five asserted a `tool.` event that did not exist, so it could not have been checking what I claimed. The
assertion was real code, the event was absent, and the pass was therefore not evidence — a gate can be green and
vacuous at the same time, which is worth remembering the next time a suite looks reassuring.

ASan caught the leak the new events introduced: the maps were built, emitted and never freed. Freed now; all three
builds are clean.

Hundred-and-thirtieth: the repair path, re-measured now that the tool events are real. The earlier diagnosis — "dispatch
stops at judge and ends the turn with no tool.call" — cannot stand, because tool.call did not exist then. With the events
in place the trace reads differently: the repair request is submitted, its answer parses as `tool=read`, the call is
dispatched, and then the *next* provider request in the turn fails with "module returned invalid CBOR".

That next request is the driver asking the provider again after the tool result, and its prompt still carries the "repair
me" marker, so the fixture matches it and answers with the vague body a second time; the provider then reports a tool
without arguments, core has already used its one repair, and something in that reply path hands the driver a payload that
marshals to nothing.

The fixture is the wrong shape for this: matching on request text cannot express "answer the first request one way and
every later one another way", and tonight it has already produced two false trails that way. The fix is the same one that
worked for the dispatch gate — count requests in the fake endpoint (a counter file) and answer by position — and the
assertion belongs in the gate only after that.

Reverted, tree green, and recorded with the trace.

Hundred-and-thirty-first: the fixture now answers the repair turn by position and the trace is unambiguous. The first
request of the turn gets the tool call; core parses `tool=read`, dispatches, and the capability runs. The *second*
provider request — the driver asking again after the tool result — matches the same marker, gets the vague body, and the
provider reports a tool with no arguments. Core has already spent its repair, so it clears the tool and passes the refusal
text through, and that is where the turn dies with "module returned invalid CBOR" rather than ending with the refusal as
text.

So the repair itself works; the fault is in what a second provider round inside one turn does to the message that follows
it. That is a turn-state question (`pending_kind`, the parked action and the reply map), not a parsing one, and it is
recorded here rather than chased further tonight: the repair is an extra beyond the note's list, and the note's list is
what the milestone is measured against.

Reverted the gate and fixture; tree green. The repair code stays with `repair_tried` bounding it to one attempt.

Hundred-and-thirty-second: the provider no longer returns in silence. When a caller invokes it without a prompt it used
to return without writing anything, which surfaces upstream as "module returned invalid CBOR" with no clue where it came
from — the same failure shape as the oversized body, and the same lesson written down two entries after that one. It now
answers with an empty text and status 0, so the caller gets a reply it can act on.

Not verified by a unit test: my first attempt called the guest with an empty map and read a status that decoded as
2^39, which is not what the encoder was told to write, so I reverted the test rather than ship an assertion I could not
explain. The guard is a strictly better failure mode than silence, and the test needs a second look at how the reply is
marshalled for a zero-length text.

Hundred-and-thirty-third: the 2^39 was my test, not the guest. Dumping the guest's bytes for a promptless call shows exactly
`a2 64 "text" 60 66 "status" 00` — a two-pair map with an empty text and a zero status, fifteen bytes, precisely what the
encoder was told to write. Decoded, the status is 0. The earlier assertion that read 2^39 was reading something other than
what it thought; the guard was correct all along, and is now covered by a real assertion on the reply's status and empty
text.

That is the third time this session that a measurement apparatus, not the product, was the fault — the pipe that looked
like a hang, the fixture that answered by content, and now a decode that read the wrong item. The pattern is consistent
enough to be a rule for next time: when a number is impossible, print the bytes before theorising about the layer below.

Hundred-and-thirty-fourth: the repair path with ordered answers shows the shape of the real fault. The first request gets
the vague body, core asks its one repair, the second request gets a valid tool call, the capability runs — and then the
driver asks the provider *twice more* in the same turn (two further replies logged), after which the turn dies with
"module returned invalid CBOR" and an error name of "unknown".

So the repair works and the capability runs; what breaks is the number of provider rounds the driver asks for after a
`{kind:"tool"}` message, and the eventual payload that marshals to nothing. That is a driver-state question: the tool
message is a new kind for that guest, and after it the driver appears to fall back to its provider path without a prompt.

The provider guard (entry 132) makes a promptless call answer instead of returning silently, so the empty reply now comes
from somewhere else; the next step is to log which module returns zero bytes, which means naming the instance in `invoke`'s
error rather than leaving "module returned invalid CBOR" anonymous — and that is worth doing regardless of this bug,
because the message has cost me two round trips tonight.

Reverted the gate and fixture; tree green.

Hundred-and-thirty-fifth: the byte count paid for itself immediately and the repair bug is now located to a single call:
the turn dies on a guest that receives **1673 bytes and writes nothing**. That size matches the provider payload after the
tool result — prompt plus history plus the repair instruction — so the silent guest is almost certainly the provider, and
the guard added in entry 132 does not cover that path, because the payload *does* carry a prompt.

Which means the provider is returning early somewhere else: most plausibly its key loop rejects a value it cannot name
(the history array, or a key whose value type it does not expect) and returns without writing, exactly as it did for the
oversized body. The next step is the same treatment as the guard: every `return` in the guest's parse loop should leave a
well-formed error reply behind, and the guest needs a test that feeds it a payload with a history array and an unexpected
key.

Stopping on the repair for now. It is an extra beyond the note's list, it has now cost four rounds, and the same effort
spent on M6 buys far more of the objective. Reverted; tree green.

Hundred-and-thirty-sixth: the client is a library. `wd_client`, connect, handshake, the frame reader, the send helper and
the request call moved from `src/cli/main.c` into `src/client/client.c` with a `client/client.h`, and the CLI now links
against them. `wd_client_read_frame` and `wd_client_send_item` are exported because the CLI's own follow and resume loops
need them; the rest stayed static.

Everything that uses the CLI is the regression test and nothing changed: the suite is green in all three builds, and a
live `session list` and `stats` against the running daemon answer as before.

This is the first slice of M6 — until now nothing but `src/cli/main.c` could speak the protocol, so a native client or an
Android app had nothing to link. The next slice is `clients/native/` with its own gate, then the phone.

Hundred-and-thirty-seventh: a second consumer exists. `clients/native/main.c` builds as `ward-native` and speaks the
protocol through the new library: `create` answers a session id, `prompt` sends a turn, `resume` reads frames until
`turn.end` and prints the deltas. Smoke-tested against the running daemon — a session was created (`c03e623271f4a4e2`)
and a prompt accepted; the turn failed only because the local router cannot load its model, which is the box, not the
client.

No gate yet, and the document says so. The gate is the next slice: a daemon on the fake endpoint, the three commands run
as subprocesses, and an assertion that `resume` prints the echoed prompt before `turn.end`.

Hundred-and-thirty-eighth: the gate litter is fixed at its source. Two faults compounded.

First, the fake endpoint's forked server and its per-connection workers carried no `PR_SET_PDEATHSIG`, so when a gate was
killed mid-run — by an assertion, a timeout or me — they survived, were reparented to init, and kept the gate's argv. That
is why `ps` showed seven `./out/gate_tool` processes: they were fixture servers wearing the gate's name, with their own
zombie workers underneath. Both forks now set PDEATHSIG, so they die with the gate.

Second, and this is the one that hid the first: **the test binaries were never rebuilt when a header changed**, because the
Makefile's `-include` line listed the core, CLI and native dependency files but not the tests'. `tests/fake_endpoint.h` had
been edited at 13:32 while `out/gate_tool` still dated from 13:25, so every verification I ran against the "fix" was
running the old binary. The tests' `.d` files are included now.

Verified the way the leak was found: start `gate_tool`, `kill -9` it after three seconds, then enumerate every process
whose executable name starts with `gate_` — zero remain. Before the fix the same check found seven.

A hanging gate is the same class of problem: `gate_native` blocked for its whole timeout while it was being written, so the
file is removed rather than committed. Its next attempt needs a hard `alarm()` so a stall fails instead of occupying a
slot for five minutes.

Hundred-and-thirty-ninth: the native gate fails fast now and fails for a real reason. It no longer hangs: the child is
forked with `PR_SET_PDEATHSIG` and an `alarm(20)`, so a stalled command is killed and the gate reports instead of occupying
a slot. What it reports is that `ward-native prompt` never returns against the gate's daemon — the child dies at the alarm
with empty output — while the same command against the running service answers promptly (it printed "turn failed" when the
router could not load a model). So the fixture wiring for this gate is not right yet: something about the way the daemon
reaches the fake endpoint differs from `gate_tool`'s setup, and the provider request never completes.

Removed rather than committed, because a gate that always fails is as useless as one that hangs, and the tree stays green.

The other thing the run showed: `wd_cgroup_leave` builds its path from `/proc/self/cgroup`, which ends in a newline, so the
`sudo tee` fallback received `.../cgroup.procs\n` and failed — the daemon cannot leave its cgroup on shutdown, so the test
cgroup is left behind for the sweep. Trimming the newline is a two-line fix for the next round.

Hundred-and-fortieth: the native gate got further and exposed a different question. With `endpoint` written into the
fixture's `models.toml` — not relying on the inherited `WARD_ENDPOINT` — the daemon does reach the fake endpoint: the log
shows the SSE events, the provider's reply parsed, and the turn's drive() beginning. What still does not happen within
forty-five seconds is the *response to `session.prompt`*: the client blocks, the alarm kills it, and the gate reports an
empty capture.

So the remaining question is not connectivity but ordering: either the daemon answers `session.prompt` only after the turn
runs to completion, or the turn itself does not finish. That is worth knowing beyond the gate — a client that must wait for
a whole turn before its prompt is acknowledged is not the behaviour the protocol describes.

Removed again, with the finding recorded. The next attempt should prompt from a *second* process while a first one holds a
subscription, exactly as `gate_tool` does, and see whether the response arrives then; that distinguishes "the daemon defers
the reply" from "the turn never ends".

Hundred-and-forty-first: reading the handler settles half the question and sharpens the other half. `handle_session_prompt`
calls `wd_turn_prompt` **synchronously** and only then sends the response, so a client waits for the turn to reach its first
park. That is by design and matches the CLI, which returns as soon as the provider request is parked.

In the gate that park never happened: the log shows one provider reply parsed, then nothing for forty-five seconds — no second
reply, no error, no turn.end. So the turn was stuck after the provider answer, inside the driver, rather than waiting on the
network. Two candidate causes remain and they are distinguishable in one run: either the driver guest loops on this reply
(an empty-arguments tool result, or a text it cannot classify) or the daemon died silently while holding the socket.

The next probe is cheap: run the same turn through the CLI against the same fixture and see whether the CLI returns. If it
does, the difference is the client; if it does not, the driver is the one to instrument, with the same byte-printing
discipline that resolved the 2^39.

Hundred-and-forty-second: the native gate passes, and it found a real daemon bug. The trace narrowed it to liveness: the
daemon stayed responsive while the prompt never answered — a second `create` succeeded during the stall — so nothing was
wedged, the response simply never left the queue.

The cause: `handle_session_prompt` was the one handler that queued its response with `conn_send_cached` and did **not** call
`conn_arm` afterwards. A connection that had already been armed by an earlier call — `session.resume`, as `gate_tool` does —
flushed the queued response; a connection that had only ever connected and prompted, as `ward-native` does, never got an
EPOLLOUT registration and waited forever. Error responses went through a different path, which is why a failing turn answered
promptly and a succeeding one did not.

One line fixes it, and the gate now drives a full session through `ward-native`: create, prompt, resume, and the echoed text
before `turn.end`. The client workaround I tried first — subscribing before prompting — is reverted, because it papered over
the bug rather than fixing it.

The gate also needed three build changes: tests now link `src/client` (so a gate can act as a client), `WARD_NATIVE` is
exported into the suite, and `make test` depends on the native binary so every build directory has one.

Hundred-and-forty-third: a golden transcript exists. `WARD_DUMP_FRAMES=1` makes the client print every frame it sends and
receives as hex, and `docs/transcript.md` records a captured `create` exchange: the daemon's `welcome`, the
`session.create` request and its response, byte for byte.

That is the first of the three things the Android client needed. It is also a cross-check the C client can keep using: the
bytes are the contract, and a re-implementation can diff against them instead of reading three files and hoping.

The dump also exposed a build fault worth noting: `ward-native` was not relinked when `src/client/client.c` changed, because
its link rule listed the native and core objects but not the client's. The binary I had been testing was older than the
library it linked — the same class of mistake as the missing test dependency files, in the same Makefile.

Hundred-and-forty-fourth: the conformance harness exists. `tests/transcripts/create.txt` holds the captured frames — the
welcome, the `session.create` request and its response — and `tests/conformance_test.c` decodes each one with `wd_decode`
and re-encodes it, asserting the result is byte-identical. Three frames round-trip exactly.

That is the third thing the Android row needed, in its first form: the bytes are now a fixture a JVM test can read as
easily as this one does, and "the client produces these bytes" becomes an assertion rather than a hope.

The rule the repository applies is still not satisfied for Android — the row is not `delivered` until a client under test
replays the fixture — but the fixture and the harness are the part this repository can own without a device.

Hundred-and-forty-fifth: the Android client's shape is chosen — NDK reuse of `src/client` — and its interface exists.
`clients/android/ward_jni.c` exposes `connect`, `call` and `close` over `wd_client`, with CBOR crossing as byte arrays so
the Kotlin side never needs a codec of its own. `make android-lib` builds `out/libwardclient.so` against the host compiler
and the JDK's `jni.h`, which proves the layer compiles and links.

What it does not prove is behaviour: the library has been built, not exercised. And a real Android build needs libcbor
cross-compiled for the target ABI — the one dependency the client does not already carry.

The row stays `missing` under the repository's rule, and `docs/m6.md` says exactly what remains: an APK, a device, and the
conformance fixture replayed from Kotlin.

Hundred-and-forty-sixth: the JNI layer does real work. `clients/android/uk/xa0/ward/WardClient.java` wraps it, and
`make android-test` compiles the class, loads `out/libwardclient.so` and creates a session against the running daemon —
`session 796c33b14a79b8ed`. That is the interface behaving on a JVM, which is the environment Android's JNI uses.

Getting there cost one SELinux lesson: `javac -d ~/tmp/...` fails with "could not create parent directories" while `touch`
in the same directory succeeds, because `java_t` may not write `user_tmp_t`. `ausearch` showed the denial in one command.
Building into `out/` inside the repository works, and the Makefile target does that.

The row is still not `delivered`: what remains is libcbor for the Android ABI, an APK, and the conformance fixture replayed
from a device. What changed is that the interface is no longer unexercised code.

Hundred-and-forty-seventh: ward listens on TCP when asked. `WARD_TCP_BIND=127.0.0.1:PORT` adds a second listener to the same
event loop, prints `tcp 127.0.0.1:<port>` beside the Unix socket, and accepts those connections into the same machinery. The
client understands `tcp://host:port`, so `ward-native` drives a session over it: `session e3cfdd3610edc5b6` created and
resumed through the TCP path, and `gate_native` now asserts that too.

The credential check is the part that needed judgement. ward authenticates a client by `SO_PEERCRED` and requires the uid to
match its own; a TCP peer has no such credential, so the first attempt was refused with "handshake refused". The rule added
is deliberately narrow: a TCP connection is accepted only when it comes from `127.0.0.1` **and only on the TCP listener**.
That is what makes `adb reverse` viable — the phone's connections arrive from adbd on the host as loopback — while keeping
the Unix socket's uid check exactly as it was.

This is the transport the Android row needed. What is still missing is the library cross-compiled for the phone and an APK.

Hundred-and-forty-eighth: the phone drives a real session. `clients/android` now holds a minimal app beside the JNI layer and
the Java wrapper, and on the disposable handset it printed

```
I ward    : session dd8ae525d9994557
```

That is M6's acceptance line, and it needed the three things built in order: libcbor cross-compiled for arm64-v8a with the
NDK, the JNI library built at API 28 (core wants `getrandom`), and the APK assembled with aapt2, javac, d8, zip, zipalign
and apksigner — then `adb reverse tcp:45872 tcp:45872` so the device's loopback arrives at the daemon's TCP listener.

Four traps are recorded in `docs/android.md`: the manifest needs an explicit `targetSdkVersion` or the installer refuses it;
build-tools' `d8` is class file 55 and will not run under the system `java` 8; `javac` cannot write into `~/tmp` (SELinux,
`java_t` against `user_tmp_t`); and `getrandom` needs API 28.

`docs/parity.md` now has no missing row: Android is delivered, with the logcat line as its evidence.

Hundred-and-forty-ninth: `session.file_read` exists. `wd_file_read` resolves the path against the session's cwd, refuses
anything that resolves outside it (both `../` and an absolute path elsewhere), refuses a non-regular file, and refuses a
file larger than 64 KiB rather than truncating it — the rule that a reader should not quietly return part of a file.

Two gates: `files_test` covers the helper directly (content, escape by `../`, escape by absolute path, missing file, cap),
and `gate_native` calls the method over the socket for both the success and the refusal, so the protocol path is measured
rather than assumed. All three builds green.

The desktop client's files dock can now answer: the method returns the bytes and the size, and the refusal is a message a
UI can show rather than an empty pane.

Hundred-and-fiftieth: `session.file_read_blob` reads binary in bounded chunks — 1 MiB per call, offsets up to 4 MiB, the
same cwd-rooted and regular-file refusals as the text method, and `data` as a CBOR byte string. The text method keeps its
64 KiB cap untouched: a lightbox should not be the reason a text read starts returning megabytes.

`files_test` covers a mid-file range, an escape by `../`, and the total cap; the JSON/byte-string helper is `wd_bytes_len`
in the codec. All three builds green.

Hundred-and-fifty-first: `approval.list`. The approval table already held the state — session, call, epoch, expiry — and
only exposed a count, so a client could not mark a session it was not following. `wd_approval_list` returns the pending
entries and the method serves them globally: `{approvals: [{session, call, epoch, expires_ms}]}`, absolute expiry in epoch
milliseconds to match the protocol's other time fields.

Two things deliberately left out and said so: the tool name and the reason are not in the table, because they are emitted on
`approval.requested` and storing them would change a signature used in three places for a field the client already receives.
If a reconnect needs them, that is the follow-up, and it is additive.

`approval_test` now asserts a pending entry appears with its expiry and disappears exactly at it. Suite green.

Hundred-and-fifty-second: approvals remember why. `wd_approval_request` now takes the tool and the reason, the entries keep
them, and `approval.list` returns them beside the session, call, epoch and expiry, so a client that reconnects without
replaying can say "write: write a file in the session directory" instead of "approval required". The parked action already
carried the tool name; `wd_turn_pending_tool` exposes it, and both server call sites pass it through.

`approval_test` asserts an entry with both fields. Suite green in all three builds, and the protocol document no longer says
the fields are missing.

Hundred-and-fifty-third: a prompt sent while a turn runs is queued, not refused. `session.prompt` answers
`{session, queued: true, ahead: N}`, appends `session.prompted` immediately so the transcript shows the user row, and
starts the turn when the running one ends; the queued item carries `announced` so `start_session_turn` does not emit the
same two events twice. A queue deeper than eight answers the busy error rather than a false queued answer.

The gate came next: `gate_native` now creates a second session against a daemon holding `WARD_FAKE_DELAY_MS=1500`, prompts
it twice, and asserts the second answer is `{queued: true, ahead: 0}` while the first turn is still running — so the path is
measured rather than assumed.

Earlier, honestly, this path had **no gate of its own**. The behaviour needs a turn that is still running
when the second prompt arrives, which means a slowed provider; `gate_tool` has the delay machinery and is where the
assertion belongs. Recorded rather than claimed.

Hundred-and-fifty-fourth: steer. `session.prompt` with `steer: true` while a turn runs appends the text to that turn s
next provider request, in order, and answers `{session, steered: true}`. `provider_phase` is the single boundary every
provider call passes through, so delivery lives there: pending steers are merged into the prompt before the provider sees
it, and cleared. It is finish-then-read, never an interruption; a ninth steer for one turn answers a busy error rather than
dropping one silently, and with no turn running the flag is ignored so the prompt just runs.

`gate_native` asserts the steered answer on a session of its own — the queue test taught that lesson twice: a shared
session makes a later assertion fail for the wrong reason.
