# The ward cgroup

ward runs inside its own cgroup subtree, builtin: it writes the kernel's own
control files, spawns no helper and needs no cgroup manager.

```
/sys/fs/cgroup/ward/            fence, no processes
/sys/fs/cgroup/ward/harness/    the daemon, 2 GiB high and 2 GiB max
/sys/fs/cgroup/ward/test/       one leaf per gate run, swept on the next run
```

The shape mirrors dsh's: a fence that holds children but no processes of its
own, and a leaf that holds the work. A cgroup with children may not hold
processes, so the fence must stay empty — that is why the OpenRC supervisor is
never inside `/ward`: moving one process does not move its parent, and ward
checks the supervisor's cgroup before it enters and refuses with a clear
message if the supervisor is inside the fence.

## Configuration

| variable | default | meaning |
|---|---|---|
| `WARD_CGROUP` | `/sys/fs/cgroup/ward` | fence path; `off` disables the whole thing |
| `WARD_CGROUP_MAX` | `2G` | `memory.max` of the leaf |
| `WARD_CGROUP_HIGH` | same as max | `memory.high` of the leaf |
| `WARD_CGROUP_PIDS` | `256` | `pids.max` of the leaf |
| `WARD_CGROUP_SUDO` | unset | `0` forbids the privileged fallback |

Sizes take a `k`, `m` or `g` suffix.

## Why a privileged fallback exists

cgroup v2 will only migrate a process if it can write to the `cgroup.procs` of
the closest common ancestor of the source and destination cgroups. A user
service normally starts in the session's cgroup (here `/dsh/svc/session-…`),
whose common ancestor with `/ward` is the root cgroup — root-owned. So the
move of ward's own pid needs privilege once; ward writes the pid directly when
it can, and otherwise runs `sudo -n tee <path>` with the pid on stdin.

The one-time setup that makes the subtree usable is done by root:

```
sudo mkdir -p /sys/fs/cgroup/ward
sudo chown -R "$USER":"$USER" /sys/fs/cgroup/ward
```

## Tests

Every gate gives its daemon its own fence, `/ward/test/gate-<pid>`, with the
same `harness` leaf and the same limits, and removes it on the way out. A gate
killed with SIGKILL cannot clean up after itself, so each gate sweeps stale
`gate-*` leaves at startup.

Verified on this box: the daemon sits in `/ward/harness` with
`memory.max = memory.high = 2 GiB`, `memory.swap.max = 0`, `pids.max = 256`,
and `supervise-daemon` remains in its session cgroup outside `/ward`.
