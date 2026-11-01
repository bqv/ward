# Running ward as a user service

The daemon is one `ward up`. On this box that is an OpenRC *user* service: it runs
as you, its tree lives in `~/.config/rc` (which links to `~/etc/{init.d,conf.d}`),
and nothing in it needs root.

    cp contrib/openrc/ward.initd   ~/etc/init.d/ward
    cp contrib/openrc/ward.conf.d  ~/etc/conf.d/ward
    chmod +x ~/etc/init.d/ward

    rc-service --user ward start
    rc-service --user ward status
    rc-update  --user add ward default

Clients reach it through `$WARD_SOCKET`. OpenRC does not set `XDG_RUNTIME_DIR`,
so the init script decides: `$XDG_RUNTIME_DIR/ward.sock` when that directory
exists and is writable — the private per-session runtime directory — and
`/tmp/ward-$UID.sock` otherwise. The fallback carries the uid; a bare
`/tmp/ward.sock` would collide between users. The pidfile follows the same
choice.

## What the configuration must set

- `WARD_BIN` — the daemon, `out/ward` while developing, installed elsewhere after
  `make install`.
- `WARD_MODULE_DIR` — the directory holding `engine.wasm`, `driver.wasm` and the
  rest. Without it the daemon starts and then refuses every prompt with *cannot
  read module*, and the scheduler disables itself with *schedule.wasm is
  missing*. This is the setting that is easiest to forget because the daemon
  still looks healthy from `session list`.
- `WARD_STATE` — sessions, `models.toml`, `labels.db`, the pki directory.
- `WARD_SOCKET`, `WARD_LOG_DIR`, `HOME`.

## Paths the daemon writes

`start_pre` creates the state and log directories and clears a stale socket, but
only when no live pid holds it: a crash leaves the socket inode behind and
`bind(2)` refuses a path that exists. `stop_post` removes the pidfile and any
socket the daemon did not unlink itself.

## A prompt needs a model

A running service answers `session.list` and every method that does not call a
model. `ward -p TEXT` fails with *the provider asked for no endpoint* until
`models.toml` names a model in `WARD_STATE`, which is deployment rather than
service configuration.
