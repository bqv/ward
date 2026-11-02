# ward-gui

A native desktop client for a local [ward](../../README.md) daemon: Qt Widgets,
C++ and no Qt Quick. It is the dsh-native shell repurposed to speak ward's
protocol, copied into this tree so the client and the daemon share one wire
definition.

## What it talks to

Three transports, one at a time, chosen AT LAUNCH with `--transport`:

| | |
|---|---|
| `WardClient` (default) | ward's binary protocol: a 22-byte frame header and a CBOR body over `AF_UNIX` or loopback TCP. Frames, the CBOR codec and the default socket path are the same C translation units the daemon and `ward-native` use, compiled into this binary as `wardwire`; the client does not re-implement them. |
| `DshClient` | The DeepSeek Harness Remote API over WebSocket. Kept so the shell can still be compared against the host it was written for. |
| `AcpClient` | JSON-RPC over the stdio of a `dsh --profile acp` child. Chat and tools only. |

`ClientFacade` binds the shell to whichever transport was selected. Nothing
switches transports at runtime and nothing falls back by itself: a transport
that is not there leaves the window saying so.

## Build

Qt 6 (6.11.2 on this box) with Widgets and WebSockets, CMake >= 3.16, Ninja,
libvterm and libcbor found through `pkg-config`.

```sh
cmake -S . -B build -G Ninja && cmake --build build
```

This builds three binaries:

| binary | purpose |
|---|---|
| `ward-gui` | the shell |
| `ward-gui-probe` | a headless protocol probe: handshake, `session.list`, create, prompt, approvals and replay, with no display |
| `libwardwire.a` | ward's `frame.c`, `codec.c` and `util.c`, compiled for the client |

## Run

```sh
./build/ward-gui                                # ward, at $WARD_SOCKET or the default path
./build/ward-gui --socket tcp://127.0.0.1:45871 # ward over loopback TCP
./build/ward-gui --transport dsh                # the old DSH WebSocket transport
./build/ward-gui --transport acp                # the old ACP stdio child
```

`--socket` accepts a Unix path or `tcp://host:port`; `WARD_SOCKET` is consulted
when the flag is absent, then ward's own default `$XDG_RUNTIME_DIR/ward.sock`
(or `/tmp/ward-$UID.sock`).

The probe is the display-free path and the client's gate:

```sh
./build/ward-gui-probe --socket /run/user/1000/ward.sock --prompt "hello"
./build/ward-gui-probe --socket /run/user/1000/ward.sock --approve --prompt "/write note.txt hi"
./build/ward-gui-probe --socket /run/user/1000/ward.sock --session <id>
```

It prints the assistant text on stdout and every event kind on stderr; `--approve`
answers each `approval.requested` with `allow`.

## What ward-gui uses

The client speaks ward's method names and event kinds, not an adapter vocabulary:

| surface | ward method or event |
|---|---|
| roster | `session.list`, `session.updated` |
| new session | `session.create` |
| open and replay | `session.resume` with `from: 0`, ending at `replay.end` |
| prompt | `session.prompt`; `session.prompted`, `text.delta`, `turn.start`, `turn.end` |
| approval | `approval.requested` -> `approval.respond` -> `approval.resolved` |
| cancel | `session.cancel` |
| rename, pin, archive | `session.rename`, `session.pin`, `session.archive` |
| compact | `session.compact` |
| goal | `goal.set`, `goal.clear` |
| models | `model.list`, `session.set_model` |
| jobs, schedules | `job.list`, `job.kill`, `schedule.list`, `schedule.delete` |
| any other method | `wardCall(method, params)` is the generic door; `protocolMethods()` lists the frozen table |

Events the shell has no control for (`terminal.output`, `turn.timings`,
`job.updated`, `session.modules`, ...) still reach `eventReceived(kind, payload)`
and are never silently dropped by the wire layer.

## Not adopted yet

- **QUIC.** `WardClient` connects over `AF_UNIX` and loopback TCP. ward's QUIC
  transport, device certificates and pairing are not in this client yet; the
  daemon's own QUIC client (`src/net/quic.c`) is the code to reuse.
- **Terminal, files, attachments, workspaces, plugin inventory, permission
  presets.** ward has no matching client surface or no matching method in this
  tree; the properties exist and answer empty so the widgets degrade honestly.
- **Deferred new-session seats.** A new session is created immediately rather
  than on the first send.

## Layout

The shell, the theme, the rail, the transcript, the terminal emulator and the
document panes are copied from dsh-native. The ward-specific code is:

| file | role |
|---|---|
| `src/WardClient.{h,cpp}` | the ward transport: connect, handshake, framed CBOR requests, event routing, the facade surface |
| `tools/ward_probe.cpp` | the display-free gate |
| `CMakeLists.txt` | builds `wardwire` from ward's `src/proto` and `src/core/util.c` |

## Licence

MPL-2.0. See `LICENSE`.
