# ward module ABI

Version 1, frozen at M1. This file is the guest contract: export names, argument
order, memory ownership and the import set are a compatibility surface for every
module, first-party or not. Changing one is an ABI version bump.

A module is a core WebAssembly module, not a component. It may target
`wasm32-wasip1` or be freestanding; either way it owns no file descriptors, no
sockets and no children of its own. Everything it needs arrives through
`ward.cap`.

## Exports

All pointers are guest `i32` offsets into the module's exported linear memory.

| export | signature | returns |
|---|---|---|
| `memory` | — | the module's linear memory, exported so the host can read it |
| `ward_alloc` | `(i32 len)` | `i32` pointer to `len` writable bytes, or `0` |
| `ward_call` | `(i32 ptr, i32 len, i32 out)` | nothing |
| `ward_state_export` | `(i32 out)` | nothing, optional |
| `ward_state_import` | `(i32 ptr, i32 len)` | `i32`, zero on success, optional |
| `ward_state_migrate` | `(i32 schema, i32 ptr, i32 len, i32 out)` | nothing, optional |

`memory`, `ward_alloc` and `ward_call` are required; the host refuses a module
without them at load, before any code runs.

A call writes its reply into `out`, which is eight bytes the host allocated with
`ward_alloc`: a little-endian `{u32 ptr, u32 len}` pair naming the reply in the
guest's memory. The host copies the reply out immediately, so the guest may
reuse that buffer as soon as `ward_call` returns.

Requests and replies are CBOR. The host bounds-checks every pointer and length it
is handed, in both directions, against the memory's current size; an out-of-range
span is an error, never a read or a write.

## The one import

A module that needs anything from core imports exactly one function:

```
ward.cap(i32 ptr, i32 len, i32 out) -> ()
```

The buffer at `ptr` is a request: a little-endian `u32` capability-name length,
the name, then the payload. The reply is written to `out` in the same
`{u32 ptr, u32 len}` form as `ward_call`.

A capability not named in the module's manifest is refused at instantiation. A
capability named but not granted and called anyway traps with
`capability denied: <name>`. Every crossing is a place the host can log and
audit.

`engine` is the exception and proves it: it is loaded as a **pure** module, which
means the host refuses it if it has any import at all, `ward.cap` included. It is
a transition function over bytes and has no capabilities to ask for.

No other imports are permitted. A module may import
`wasi_snapshot_preview1` functions, which core links from its own WASI
configuration; anything else — a second custom module, a second host function —
fails the load.

## net.request

The provider's one capability returns bytes, not CBOR, because the body is a stream
rather than a value: the reply is a little-endian `u32` HTTP status followed by the
response body. The request is `{path, body}`; core owns the host, so a guest names a
path and never an origin, and core refuses an endpoint that is not loopback.

## fs.read and fs.write

The first-party `tools` module reads and writes files through core, never directly: the
request is `{path}` for `fs.read` and `{path, text}` for `fs.write`, and the reply is
`{ok: true, text}` or `{ok: true, written}` on success and `{ok: false, error}` otherwise.
A path is relative to the turn's tool root, may not contain `..` or start with `/`, is
resolved with `realpath` and checked against the root again after resolution, and the file
is opened `O_NOFOLLOW`. Reads stop at 64 KiB and refuse a file containing a NUL byte.

The tools module's only inputs are `{tool, args}`; it forwards to the capability named by
the tool and returns the capability's reply unchanged. Core decides what a capability may
do; the module decides which one to ask.

## exec.run

`exec.run {command, timeout_ms?}` runs one command in the turn's tool root and returns
`{ok, status, timed_out, text}`; `text` is the combined output, capped at 64 KiB, and a
command that exits with no output still says what happened. Core splits the command on
whitespace itself — there is no shell, so no quoting, globbing or redirection — resolves
the program through `/usr/local/bin`, `/usr/bin` and `/bin`, and runs it with a scrubbed
environment (`PATH`, `LANG`, `LC_ALL` and nothing else), in its own session, with a
default ten-second timeout that the caller may raise to ten minutes. A timeout kills the
whole process group.

This is core's process spawning, which the README reserves for core; the worker that will
own the sandbox for a session's long-lived children is not built yet, so an `exec` call
today is one short-lived child of the daemon.

## policy.check

`policy.check {tool}` returns `{decision}` where the decision is `allow`, `ask` or `deny`.
The first-party `policy` module is pure — no imports at all, enforced like `engine` — so the
rules it holds cannot reach a file, a socket or the model. `driver` asks through this
capability before every tool call; core dispatches the call into the module and turns `ask`
into an approval, which is the only part of the decision core owns. An unknown answer is
treated as `deny`.

## Limits

Core owns the fences, and a module cannot widen them:

- **Fuel** is charged per call and reset before each one. Exhaustion traps with
  `fuel exhausted`, and the instance survives: the next call starts with a full
  budget.
- **Memory** growth is capped per store by the host's store limiter. An
  instantiation that needs more than the cap fails before the module runs.
- **Instances and memories** per store are capped as well, so a module cannot
  spawn its way around the budget.

## State

`ward_state_export` and `ward_state_import` are how a session survives a module
upgrade. The blob is opaque CBOR; core stores it and hands it back to the same
module, or to the next version of it through `ward_state_migrate`.

State is the only thing that outlives a call. Anything a module keeps in linear
memory is lost when its instance is dropped, which is deliberate: an instance is
cheap and a migrated blob is the durable thing.
