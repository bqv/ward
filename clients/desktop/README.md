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

The build always links ngtcp2 and OpenSSL through pkg-config and defines
`WARD_HAVE_QUIC`; there is no separate QUIC configuration. It also builds
`ward-pair`, which mints a pairing token over the local Unix socket and enrols a
fresh CSR over QUIC:

```sh
./build/ward-pair --socket /run/user/1000/ward.sock --out ~/.local/share/ward-gui/quic
```

A QUIC connection then uses the files it wrote:

```sh
WARD_QUIC_CERT=device.crt WARD_QUIC_KEY=device.key WARD_QUIC_CA=ca.crt     ./build/ward-gui --socket quic://127.0.0.1:4433
```

With no certificate, `quic://` is refused as an unpaired client.

This builds three binaries:

| binary | purpose |
|---|---|
| `ward-gui` | the shell |
| `ward-gui-probe` | a headless protocol probe: handshake, `session.list`, create, prompt, approvals, terminal, files, presets and replay, with no display |
| `ward-pair` | CSR, `device.pair`/`enrol`, writes `device.crt`, `device.key`, `ca.crt` |
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
./build/ward-gui-probe --socket /run/user/1000/ward.sock --terminal
./build/ward-gui-probe --socket /run/user/1000/ward.sock --read note.txt
./build/ward-gui-probe --socket /run/user/1000/ward.sock --blob note.bin --blob-len 64
./build/ward-gui-probe --socket /run/user/1000/ward.sock --list .
```

It prints the assistant text on stdout and every event kind on stderr; `--approve`
answers each `approval.requested` with `allow`. `make test` from the repository
root builds the probe and runs `tests/gate_desktop.c`, which asserts the prompt
text arrives exactly once over both the Unix socket and loopback TCP.

## What ward-gui uses

The client speaks ward's method names and event kinds, not an adapter vocabulary:

| surface | ward method or event |
|---|---|
| roster | `session.list`, `session.updated`; the last roster is cached per endpoint and painted while the live list is read |
| new session | `session.create` |
| open and replay | `session.resume` with `from: 0`, ending at `replay.end` |
| prompt | `session.prompt`; `session.prompted`, `text.delta`, `turn.start`, `turn.end`; an aborted turn adds its reason row |
| approval | `approval.list` for global marks and tool/reason; `approval.requested` -> `approval.respond` -> `approval.resolved` for the card |
| cancel | `session.cancel` |
| rename, pin, archive | `session.rename`, `session.pin`, `session.archive` |
| fork | `session.fork {id, from?}` from the rail, or from a message's Fork with `from` set to that row's event seq |
| compact | `session.compact` |
| goal | `goal.set`, `goal.clear` |
| models | `model.list`, `session.set_model`; `provider` groups the Local/Cloud model popup; a model chosen on a new-session seat travels in `session.create {model}` |
| estate | `estate.status` polled every 2 s while a window is shown; the `tenants[]`/`loads[]` document drives the load chip (loading, prefilling, generating, idle, failed) and its detail card |
| terminal | `terminal.open/write/resize/close`, `terminal.output`, eof |
| files | `session.file_read` (text), `session.file_read_blob` (CBOR bytes), `session.file_list` |
| schedules | `schedule.list`, `schedule.delete` |
| trajectory | `turn.*`, `session.prompted`, `text.delta`, `tool.call/result` |
| workspaces | `host.list` for the Add workspace folder chooser; synthetic cwd groups with locally persisted rename/delete (delete moves the sessions to Ungrouped) |
| links | Markdown anchors in the transcript, ACP and headless prose open the link dialog: browser, this window, copy |
| presets | `policy.list` / `policy.select`; chip normalises `id` to `value`; full-access requires the warning confirmation and marks the chip |
| search | `session.search_text` paging; Search messages dialog, no replay |
| ACP | `ward-acp`, JSON-RPC 2.0 over stdio: initialize, session/new, session/prompt, session/cancel, session/update, request_permission |
| jobs | `job.list`, `job.follow` retained output and `job.kill`; the jobs chip opens the viewer |
| settings | `settings.get` / `settings.set`; Settings edits `max_runs` and `debug` |
| queue | `session.prompt` -> `{queued, ahead}`; `session.queue` refreshes the authoritative rows; a refused prompt returns to the composer |
| steer | `session.prompt` with `steer: true` appends to the running turn |
| images | `tool.result.path` / `session.attachment.path` -> `attachment.read_blob` |
| any other method | `wardCall(method, params)` is the generic door; `protocolMethods()` lists the frozen table |

Events the shell has no control for (`turn.timings`, `job.updated`,
`session.modules`, ...) still reach `eventReceived(kind, payload)`
and are never silently dropped by the wire layer.

## Not adopted yet

- **New folder from the workspace picker.** ward serves `host.list` but has no method to create a
  directory, so Create reports that rather than sending a call.
- **Per-message Fork on the DSH transport.** ward's event seqs let a row offer a fork from that
  message; the retained DSH transport publishes no row seqs, so its Fork is the rail's
  whole-session one.
- **QUIC pairing dialog.** Settings > Pair a device runs `ward-pair`, stores the
  certificate paths and says to restart on `--socket quic://…`; the window does
  not switch transport at runtime.
- **Late steer gate.** The daemon carries an undelivered steer into the queue at
  turn end, but no gate yet counts the extra turn; the client does not need a
  new field.

## Layout

The shell, the theme, the rail, the transcript, the terminal emulator and the
document panes are copied from dsh-native. The ward-specific code is:

| file | role |
|---|---|
| `src/WardClient.{h,cpp}` | the ward transport: connect, handshake, framed CBOR requests, event routing, terminal, jobs, the facade surface |
| `src/JobsDialog.{h,cpp}` | the jobs roster and retained-output viewer |
| `tools/ward_acp.cpp` | the ACP face over the same WardClient transport |
| `src/SearchDialog.{h,cpp}` | the FTS search box and result list |
| `tools/ward_probe.cpp` | the display-free gate |
| `CMakeLists.txt` | builds `wardwire` from ward's `src/proto` and `src/core/util.c` |

## Licence

MPL-2.0. See `LICENSE`.
