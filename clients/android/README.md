# ward-android — the Ward Android client

The project is **ward-android**, the application is **Ward**, and the application
id is `uk.xa0.ward` (debug builds install as `uk.xa0.ward.debug`). It is a native
Jetpack Compose client for a ward daemon. There is no WebView, no host-side
plugin and no HTTP control surface in the path: the app speaks ward's own binary
CBOR protocol over one socket, exactly as `docs/protocol.md` freezes it.

<p align="center">
  <img src="docs/images/ward-approval.png" width="320"
       alt="A ward session with a /write prompt, a pending approval card, and the transcript's tool call above it">
  &nbsp;&nbsp;
  <img src="docs/images/ward-transcript.png" width="320"
       alt="The same session after Allow once: the approval grant and the tool result are in the replayed transcript">
</p>

The UI began as the DSH Android client (`bqv/dsh-android`, copied at `a74f76f`)
and was repurposed rather than reimplemented: the Compose screens, transcript
reducer, composer, approval card, drawer, terminal widgets and markdown renderer
are the same code, while the whole transport and data bridge behind them is
ward's. The old minimal JNI client kept its home in `../android-ward`; a copy of
its JNI entry points is in `jni/` for reference.

## Build

Requires JDK 17, Gradle 8.7 and an Android SDK with platform 34. The local
toolchain layout is the same as the copied project: `.toolchain/jdk-17.0.20.1+1`,
`.toolchain/gradle-8.7`, and `local.properties` pointing `sdk.dir` at the SDK.
`build.sh` owns the environment, the build lock and all caches.

```sh
./build.sh :app:assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
./build.sh :app:testDebugUnitTest    # JVM suite, including the protocol gates
```

The protocol tests read a live daemon only when the harness names one; the
ordinary suite stays hermetic:

```sh
WARD_TEST_ENDPOINT=tcp://127.0.0.1:45872 \
  ./build.sh :app:testDebugUnitTest --tests 'uk.xa0.ward.proto.*'
```

## Point it at ward

Run the daemon with a loopback TCP door and reverse it into the phone:

```sh
WARD_CGROUP=off WARD_TCP_BIND=127.0.0.1:45872 \
  ./out/ward up --state ~/.local/state/ward --socket /run/user/$(id -u)/ward.sock
adb -s <serial> reverse tcp:45872 tcp:45872
```

On first launch the setup screen defaults to `tcp://127.0.0.1:45872`; a bare
`host:port` is accepted. The daemon's own `ready` line and its TCP listener are
the authority: there is no login and no cookie because authentication is the
transport's `SO_PEERCRED` check (or, for QUIC later, a client certificate).

## The protocol layer

`app/src/main/java/uk/xa0/ward/proto/` is the implementation:

| file | what it owns |
|---|---|
| `Cbor.kt` | the documented CBOR subset, definite lengths and the 64-bit unsigned form ward's own encoder emits |
| `WardProtocol.kt` | the 22-byte little-endian frame, event/session objects, errors and limits |
| `WardConnection.kt` | socket, `hello`/`welcome`, one reader, request-id ledger, `session.resume` subscriptions, replay and reconnect |
| `WardApi.kt` | every method in the frozen table, from `session.*` to `device.*`, with ward's parameter names |

`net/DshClient.kt` and `net/RemoteMux.kt` keep the surface the copied screens
already speak. `net/WardBridge.kt` is the only translation point: a ward session
becomes a DSH list row, `session.resume` events become transcript rows, and a
ward approval becomes the `$events` waterfall the attention centre already
answers. Nothing in the screens knows which protocol is underneath.

The encoder matches the golden transcript in `docs/transcript.md`: every
unsigned integer body is emitted in the 8-byte form `ward`'s libcbor encoder
produces, text is UTF-8, maps and arrays are definite, and tags, floats and
indefinite forms are refused on decode.

## What is wired through the UI

- sessions: list, create, open, resume/replay, search, rename, fork, pin,
  archive, cancel;
- turns: prompt, streaming text deltas, turn start/end, tool calls and results,
  errors, durable replay after a relaunch; the composer's steer mode travels as
  ward's `steer: true`, and a prompt while a turn runs is ward's own queue;
- approvals: `approval.requested` parks the turn, the existing card answers
  through `approval.respond`, `approval.resolved` withdraws it, and an
  `approval.list` poll raises a card for a session this client is not following;
- models: the ward catalog is grouped by provider and `session.set_model` moves
  the chip; `model.list` is read through the array key the daemon actually sends;
- attachments: the picker's uploads become `attachment.add`, and image parts in
  a prompt are staged the same way;
- files: the Files panel lists through `session.file_list`, reads text through
  `session.file_read`, and windows binary reads through `session.file_read_blob`;
  the byte windows are reassembled into the multipart shape the copied panel
  already parses;
- workspaces: ward has no registry, so the drawer groups the roster by each
  session's `cwd` — one client-owned Workspace per directory, refreshed as the
  roster changes;
- jobs: `job.list` is polled into the copied roster and `job.kill` addresses the
  turn; output streaming is not part of ward's job surface;
- schedules: `schedule.list` becomes the Automation catalogue, with the cron
  expression and next fire time; `schedule.delete` removes one;
- goals: durable `goal.set`/`goal.cleared` events become the goal projection the
  copied composure already draws;
- modules and plugins: `module.list` is the bundle list, with a read-only switch
  because ward reloads the registry as one set; `module.reload` is available;
- policy and access: `policy.list`/`policy.select` drive the access chip through
  the DSH permission names; ward's three presets are offered, not DSH's
  full-access one;
- stats: `stats.session` becomes the session and token projections behind the
  stat pills; the provider's wall-time fields do not exist on ward's wire and are
  left at zero rather than invented;
- subagents: `subagent.updated` records the parent link and publishes the child
  row before the next roster pull;
- live status: turn boundaries publish the same `api-session/status` fact the
  copied drawer and composer already consume.

## Parity gaps, named rather than hidden

These are the differences a reader will still notice against the DSH client.
They are the report sent to the coordinating session, kept here so the
repository and the message cannot drift.

- **QUIC and device pairing are not in the app.** `device.pair`/`enrol` are
  wrapped, but the phone has no QUIC client, no CSR generation, no client
  certificate and no QR surface. The loopback TCP door over `adb reverse` is the
  supported path.
- **A Unix socket cannot be opened from an Android app.** The daemon's strongest
  `SO_PEERCRED` door is a desktop/local path; the phone uses TCP.
- **No host Workspace registry.** The drawer's groups are derived from `cwd`, so
  membership and manual ordering are client-owned rather than host state.
- **The queue has no projection.** Steer and queued prompts are accepted and
  delivered, but ward has no queue snapshot for the copied dock, so a queued
  message has no authoritative row until its turn starts.
- **The terminal panel is not device-complete.** The session seat is the one
  wired for ward; `terminal.open` creates a PTY and the output frames are routed
  into `terminal/follow`, but the copied attachment screen never reached its
  first snapshot on the handset. The host-shell seat still asks for DSH commands
  and an archived session that ward does not model.
- **Job output is not streamed.** `job.list` and `job.kill` work; there is no
  equivalent of `job/follow`'s output ring.
- **Ward has no settings document, plugin bundle switch or command catalogue.**
  `settings/describe`, `commands/list` and the writable plugin switch answer
  empty/read-only shapes; module reload is the one management action offered.
- **The full-access preset has no ward equivalent.** Ward's presets are
  `default`, `reads-only` and `workspace`; the DSH `danger-full-access` chip is
  not offered.
- **Binary reads are bounded.** `session.file_read_blob` caps one call at 1 MiB
  and 4 MiB total; the app reassembles multiple windows, but a larger file is
  refused by the daemon rather than truncated.

## Device evidence

Built and installed on the disposable handset `91e25acb`, with
`adb reverse tcp:45872 tcp:45872` and the daemon above:

- the app created and listed real ward sessions (`session.create`,
  `session.list`);
- a `/read` prompt reached the daemon, streamed its tool call and painted the
  tool row;
- a `/write` prompt raised **Approval required**; **Allow once** answered
  `approval.respond`, and the transcript then showed the tool call, the grant
  and the tool result;
- the Files panel listed `/tmp` through `session.file_list` and read file text
  and byte windows through the file methods;
- the drawer grouped four sessions under one `tmp` Workspace, derived from the
  roster's cwd;
- the composer's stat pills read `2 turns · 2 steps` from `stats.session`;
- a `/write` parked in a session the app was not following raised its card from
  `approval.list`; **Allow once** resolved it and the turn continued, which is
  the background-approval path;
- force-stopping and relaunching the app replayed the same transcript from
  `session.resume`, which is the durable-event proof.

The protocol gates live in
`app/src/test/java/uk/xa0/ward/proto/WardProtocolTest.kt` (captured golden
bytes, frame round-trip, CBOR subset and refusals) and
`app/src/test/java/uk/xa0/ward/proto/WardLiveTest.kt` (create, list, resume,
rename and archive against a live daemon).
