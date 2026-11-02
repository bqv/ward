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
./build/ward-gui-probe --socket <path> --terminal
./build/ward-gui-probe --socket <path> --read note.txt
./build/ward-gui-probe --socket <path> --blob note.bin --blob-len 64
./build/ward-gui-probe --socket <path> --list .
```

The first covers handshake, `session.create`, `session.resume`, `session.prompt`
and the `text.delta`/`turn.end` stream. The second covers `approval.requested`,
`approval.respond` and `approval.resolved`. The third covers replay from
`from: 0` through `replay.end`; `--terminal` opens a PTY, writes `echo
ward-terminal-ok` and prints the bytes that come back. Against a fixture daemon the probe returned
`hello desktop` once, wrote through an approved tool call, and replayed the same
turn without duplicating a row.

`tests/gate_desktop.c` runs the probe against a fixture daemon and asserts the
prompt text appears exactly once, first over the Unix socket and then over
loopback TCP, then drives a terminal, a text read, a blob read and a directory
list (success and an escape refusal for each), then starts ward-gui offscreen with
`--prompt` and asserts the answer. It fork/execs the probe with `PR_SET_PDEATHSIG` and `alarm(20)`
rather than using `popen`, so a stalled probe fails the gate and a killed gate
does not leave a server behind. `make test` builds the probe and runs the gate.
In a sandbox that cannot write `/sys/fs/cgroup`, run the gate directly with
`WARD_GATE_CGROUP=off`.

The gate also exposed a daemon failure rather than only a client one: a
subscriber that closed right after `turn.end` could kill the daemon with
`SIGPIPE` while it was still broadcasting the turn's tail. `setup_signals` now
ignores `SIGPIPE`, and the gate runs repeatedly without losing the fixture
daemon.

## Protocol coverage

| layer | adopted |
|---|---|
| transport | `AF_UNIX`; loopback TCP via `tcp://host:port`; both are exercised by the gate |
| handshake | `hello` / `welcome`, version 1, 5 s deadline |
| frames | all six types parsed; request, response, event and control written |
| requests | correlated by id; errors surface `name` and `message` |
| events | routed by kind and subscription; `eventReceived(kind, payload)` is the generic door |
| replay | `session.resume {from: 0}` and the `replay` control frame |
| approvals | request, respond, resolved; `approval.list` marks every session with a live card and carries tool/reason, so a followed session that reconnects without replay still names the request; `requestId` is `call:epoch` |
| permission presets | `policy.list` / `policy.select`; `id` normalised to `value`, `current` drives the access chip, `observe` stays in the description |
| terminal | `terminal.open/write/resize/close`, `terminal.output`, eof and exit state |
| session lifecycle | create (client-side lazy), list, resume, prompt, cancel, rename, pin, archive, compact, set_model |
| catalog | `model.list`, `session.set_model` |
| files | `session.file_read` (text, 64 KiB), `session.file_read_blob` (CBOR bytes, 1 MiB per call, 4 MiB total) and `session.file_list`; all cwd-rooted |
| goals, jobs, schedules | `goal.set/clear`, `job.list/kill`, `schedule.list/delete` |
| trajectory | `turn.start`, `turn.end`, `turn.timings`, `session.prompted`, `text.delta`, `tool.call/result` projected into the page |
| workspaces | synthetic groups by session cwd; ward has no registry |
| everything else | `wardCall(method, params)` plus `protocolMethods()` |

Unknown events are not dropped by the wire layer: every event reaches
`eventReceived`, and the shell renders the kinds it has controls for.

## Parity gaps against dsh-native

These are the things the desktop client had and ward-gui does not, or has not
proven yet. They are reported to the ward session as well.

1. **QUIC.** The client uses Unix sockets and loopback TCP. ward's QUIC
   transport, device certificates, pairing and `device.*` are not in the client;
   `src/net/quic.h` is the code to reuse, but it pulls ngtcp2 and OpenSSL into a
   Qt client that currently links neither.
2. **Image lightbox and uploaded attachments.** Text, blob and directory
   surfaces are wired. The image lightbox still needs an image reference in
   ward's tool-result or attachment event before `attachmentUrl` can resolve
   one, and the composer's staging surface is not wired to `attachment.add/list`.
3. **Queue/steer.** `session.prompt` is serialised; the composer's queue/steer
   mode stays as-is until a mid-turn steer has a defined model-visible meaning.
   No client-side queue is invented.

Deferred new sessions, workspaces-by-cwd, trajectory projection, the headless
`--prompt` path and permission presets are now in. QUIC remains a dependency
problem; queue/steer remains a protocol-design decision.
