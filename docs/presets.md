# Permission presets and the lightbox path

Agreed with the desktop client (`session-7b7ac6d9`). This is the specification,
recorded so the next session builds it rather than reconstructing it from a chat.

## The catalog

| id | name | rules | observe |
|---|---|---|---|
| `default` | Ask writes | read allow, write ask, edit ask, exec ask, grep allow | off |
| `reads-only` | Reads only | read allow, write deny, edit deny, exec deny, grep allow | off |
| `workspace` | Workspace | read allow, write ask, edit ask, exec ask, grep allow | require-read |

Descriptions, verbatim as the chip shows them:

- **Ask writes** — "Reads and grep allowed. Writes, edits and exec ask first.
  Observation off."
- **Reads only** — "Reads and grep allowed. Write, edit and exec are denied.
  Observation off."
- **Workspace** — "Reads and grep allowed. Writes, edits and exec ask first. A
  file must be read before it is overwritten."

`observe` is never a chip, toggle or row of its own; it is part of the
description.

There is **no fourth "full access" preset**, and that is a deliberate
difference from DSH rather than an omission. The clamp below only tightens, so a
preset whose rules all say `allow` could not turn the policy module's
`write → ask` into `allow`: the chip would change its own label and nothing
else. Making presets authoritative instead would let one turn a module `deny`
into `allow`, which is a fail-open. If the owner ever wants that mode it is its
own explicit switch with its own gate, not a row in this catalogue. The chip is two runs — a dim `access` label and the preset name —
and it changes only after `policy.select` answers. A refusal leaves the old name
and shows the daemon's message.

`policy.list` always answers `current` as a real preset id, `default` when
nothing is stored. `policy.select` answers the selected id. Both keep `id`;
the client normalises it to `value` on arrival.

## Enforcement is part of the same commit

Serving a selection the decision path ignores is the same lie as a client-side
table, one layer further away. The clamp lives in `policy_check` (`src/core/turn.c`),
which already sees `{tool}` on the way in and `{decision}` on the way out:

- map the tool to its axis: read, write, edit, exec, grep;
- a preset may only **tighten**: `allow → ask → deny`;
- it may never loosen a decision the policy module already refused, so a preset
  cannot turn a deny into an allow.

That is fail-closed, which is what the README asks for.

## The lightbox path

- `tool.result` carries `path` when the tool is about **one** file: read, write,
  edit, a single-file attachment and the like. Session-relative, canonical, the
  same string the tool was given.
- **No path for plural results** — several grep hits, a directory write. Do not
  pick a first hit and call it primary: omit it. The client treats an absent
  path as "no lightbox for this row", which is honest and avoids a second lookup.
- No `paths` array now. If a gallery ever needs one it is additive, and the
  lightbox still follows `path`.
- `session.attachment` carries `path` too — the absolute store path — on both the
  event and `attachment.list`.
- `attachment.read_blob {id, name, offset, len}` → `{path, offset, size, data}`
  reads the store, with the same byte-string shape and caps as
  `session.file_read_blob` (1 MiB per call, 4 MiB of offset). `file_read_blob`
  keeps its cwd rule hard; loosening it for one caller is how a "read anything"
  method appears later.

## Order of work

1. `policy.list`, `policy.select`, the clamp, and their gate.
2. `tool.result.path` at `perform_capability`, with a gate asserting presence for
   a single-file tool and absence for a plural one.
3. `session.attachment.path` on the event and the listing.
4. `attachment.read_blob`, store-rooted.
5. The lightbox gate: path present on both carriers, plus a blob read that
   succeeds inside the store and one refused by the cap.
