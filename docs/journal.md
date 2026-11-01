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
