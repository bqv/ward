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
  errors, durable replay after a relaunch;
- approvals: `approval.requested` parks the turn, the existing card answers
  through `approval.respond`, and `approval.resolved` withdraws it;
- models: the ward catalog is grouped by provider and `session.set_model` moves
  the chip;
- attachments: the picker's uploads become `attachment.add`, and image parts in
  a prompt are staged the same way;
- live status: turn boundaries publish the same `api-session/status` fact the
  copied drawer and composer already consume.

## Parity gaps, named rather than hidden

These are the differences a reader will notice against the DSH client. They are
the report sent to the coordinating session, kept here so the repository and the
message cannot drift.

- **QUIC and device pairing are not in the app.** `device.pair`/`enrol` are
  wrapped, but the phone has no QUIC client, no CSR generation, no client
  certificate and no QR surface. The loopback TCP door over `adb reverse` is the
  supported path.
- **A Unix socket cannot be opened from an Android app.** The daemon's strongest
  `SO_PEERCRED` door is a desktop/local path; the phone uses TCP.
- **No Workspace registry.** Ward has no workspaces, so `workspace/follow`
  answers an empty baseline and the drawer is one flat list.
- **Terminal output is not subscribed.** The method wrappers and the copied
  terminal screen exist, but `terminal.output` is not routed into
  `terminal/follow`.
- **Jobs, schedules and goals are wrappers, not screens.** `job.*`,
  `schedule.*` and `goal.*` are in `WardApi`; the copied management sheets
  answer their empty shapes.
- **Subagents are wrappers only.** `subagent.prompt`/`interrupt` exist; no
  lineage UI is wired to ward's ordinary child sessions.
- **Modules, settings, plugins, file browser and stats are stubs.** Ward either
  does not serve the DSH method or the copied screen has no ward projection.
- **Approvals from a background session are not discovered.** A card appears
  only for a session this app is following; there is no ward "list pending
  approvals" method to replay the ones raised while the app was away.
- **Queue/steer semantics are absent.** Ward serialises `session.prompt`; the
  composer's queue and steer modes do not exist in the protocol.

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
- force-stopping and relaunching the app replayed the same transcript from
  `session.resume`, which is the durable-event proof.

The protocol gates live in
`app/src/test/java/uk/xa0/ward/proto/WardProtocolTest.kt` (captured golden
bytes, frame round-trip, CBOR subset and refusals) and
`app/src/test/java/uk/xa0/ward/proto/WardLiveTest.kt` (create, list, resume,
rename and archive against a live daemon).
