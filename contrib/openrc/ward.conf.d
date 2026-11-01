# ~/.config/rc/conf.d/ward -- settings for the user service init.d/ward.

# Where the daemon binary lives.  Install it from this tree with `make install`
# and point this at the installed path, or at out/ward while developing.
export WARD_BIN="/home/user/prj/ward/out/ward"

# The wasm modules the daemon loads (engine, driver, provider, tools, policy,
# skills, judge, code).  Without this the daemon starts but every prompt fails
# with "cannot read module", so the service exports it rather than relying on a
# shell environment the init script never sees.
export WARD_MODULE_DIR="/home/user/prj/ward/out"

# State: session logs, labels.db, models.toml, the module registry cache and the
# pki directory.  This is the binary's own default; naming it here keeps the
# service's paths visible in one place.
export WARD_STATE="/home/user/.local/state/ward"

# The socket clients connect to.  Left to the init script: it uses
# $XDG_RUNTIME_DIR/ward.sock when that directory exists and is writable, and
# falls back to /tmp/ward-$UID.sock otherwise.  Uncomment to override.
#export WARD_SOCKET="/tmp/ward-$(id -u).sock"

# supervise-daemon writes ward.log / ward.err here; start_pre creates the
# directory.  Neither file is rotated -- logrotate them if they grow.
export WARD_LOG_DIR="/home/user/.dsh/logs"

# Real home: ward resolves models.toml and per-session roots from HOME, and the
# default tool root is the session's cwd.
export HOME="/home/user"

# Do NOT replace PATH here: OpenRC resolves its own helpers and supervise-daemon
# from the PATH built in /usr/libexec/rc/sh/functions.sh, and this file is sourced
# afterwards, so `export PATH=...` breaks every rc step.
