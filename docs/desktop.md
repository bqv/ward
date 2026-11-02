# The desktop client

`clients/desktop/` is `ward-gui`: the dsh-native shell copied into this tree and
repurposed to speak ward's protocol. It is a second consumer of the wire next to
`ward-native` and the Android JNI client, and it exists to prove the protocol can
carry a real UI rather than a one-command smoke test.

## What was copied, and what was not

The copy is a `git archive` of dsh-native `HEAD` at the time of the port: the
QWidget shell, theme, rail, transcript, terminal emulator, markdown and the
document panes. It is not edited in its original checkout; all work happens here.

The DSH-specific transport code (`DshClient`, `BrowserAuth`, `AcpClient`,
`AcpCompat`) is retained so the same shell can still be compared against the
host it was written for, but ward is the default `--transport`. The DSH API
contract and terminal-contract documents were removed with the copy; ward's own
`docs/protocol.md` is the contract this client reads.

## Build and run

```sh
cd clients/desktop
cmake -S . -B build -G Ninja
cmake --build build
./build/ward-gui --socket /run/user/1000/ward.sock
```

`ward-gui` depends on Qt 6 Widgets/WebSockets, libvterm and libcbor. The ward
wire layer is not a re-implementation: `CMakeLists.txt` compiles `src/proto/frame.c`,
`src/proto/codec.c` and `src/core/util.c` from the parent tree into `libwardwire.a`
and `WardClient.cpp` includes the same headers the daemon does.

`ward-gui-probe` is the display-free gate:

```sh
./build/ward-gui-probe --socket <path> --prompt "hello"
./build/ward-gui-probe --socket <path> --approve --prompt "/write note.txt hi"
./build/ward-gui-probe --socket <path> --session <id>
```

The first covers handshake, `session.create`, `session.resume`, `session.prompt`
and the `text.delta`/`turn.end` stream. The second covers `approval.requested`,
`approval.respond` and `approval.resolved`. The third covers replay from
`from: 0` through `replay.end`. Against a fixture daemon the probe returned
`hello desktop` once, wrote through an approved tool call, and replayed the same
turn without duplicating a row.

## Protocol coverage

| layer | adopted |
|---|---|
| transport | `AF_UNIX`; loopback TCP via `tcp://host:port` |
| handshake | `hello` / `welcome`, version 1, 5 s deadline |
| frames | all six types parsed; request, response, event and control written |
| requests | correlated by id; errors surface `name` and `message` |
| events | routed by kind and subscription; `eventReceived(kind, payload)` is the generic door |
| replay | `session.resume {from: 0}` and the `replay` control frame |
| approvals | request, respond, resolved; `requestId` is `call:epoch` |
| session lifecycle | create, list, resume, prompt, cancel, rename, pin, archive, compact, set_model |
| catalog | `model.list`, `session.set_model` |
| goals, jobs, schedules | `goal.set/clear`, `job.list/kill`, `schedule.list/delete` |
| everything else | `wardCall(method, params)` plus `protocolMethods()` |

Unknown events are not dropped by the wire layer: every event reaches
`eventReceived`, and the shell renders the kinds it has controls for.

## Parity gaps against dsh-native

These are the things the desktop client had and ward-gui does not, or has not
proven yet. They are reported to the ward session as well.

1. **QUIC.** The client uses Unix sockets and loopback TCP. ward's QUIC
   transport, device certificates, pairing and `device.*` are not in the client;
   `src/net/quic.h` is the code to reuse.
2. **Terminal.** ward serves `terminal.open/write/resize/close` and
   `terminal.output`, but `WardClient` publishes no terminal surface, so the
   dock says "no terminal" rather than opening one.
3. **Files and attachments.** ward has no `file.read`; the files dock and image
   lightbox therefore answer empty. `attachment.add/list` exists in ward, but
   the composer's staging surface is not wired to it.
4. **Workspaces.** ward has no workspace registry; the rail groups by nothing
   and the facade reports `hasWorkspaces = false`.
5. **Permission presets.** ward's policy is a server-side rule table, not a
   preset catalog; the access chip is absent and `/permission` answers why.
6. **Trajectory page.** A tool call produces a minimal ledger entry; ward's
   richer journal has no equivalent projection in this client yet.
7. **Deferred new sessions.** dsh-native defers `session.create` until the
   first send. `WardClient` creates immediately, so a New Session press leaves a
   roster row even if nothing is sent.
8. **Headless `--prompt`.** dsh-native's `--prompt` is ACP-only; ward-gui has no
   equivalent path in the shell. The probe is the display-free path instead.
9. **No `make test` gate.** The probe is built and was run by hand against the
   fixture daemon, but it is not yet a target in the top-level suite. Under this
   repository's rule that makes the row partial, not delivered.
