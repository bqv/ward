# Upstream work not yet absorbed

Every upstream commit this project previously set aside, with the ward
equivalent it needs and its state. "Skipped" is not a verdict here; each row is
either being ported or needs a named daemon/protocol capability built first.
Sources: dsh-linux `30981c4..954e32d` (plus `7ebbdfe`) and dsh-android
`40211e4..ccf2d8d`.

## Client work that needs no new daemon surface

| upstream | what it does | ward equivalent | state |
|---|---|---|---|
| dsh-linux 04e06d1, d58a511 | rename or unregister a workspace from the rail | workspaces are cwd groups; store a client-side title alias and a hidden set, clear a hidden group's sessions into the ungrouped bucket | in progress |
| dsh-linux 96da4a4, 1f32f6d | steer a queued message from its own row | row-level steer once queue items carry ids (see daemon rows) | daemon ready: ids and `session.queue.steer` exist |
| dsh-linux 91ebfbf, 4d31c1d, bfe2bed, 039482f, 9a07a9e, 6f79f12, b3003fe, 0d03e30 | two-column model menu with captions and scrolling | group `model.list` by its `provider` field and adopt the popup geometry fixes | in progress |
| dsh-linux 63ae930, ce2bad3 | keep the last roster painted while the list loads | cache the roster keyed by endpoint, paint on startup | in progress |
| dsh-linux 8753fd3, f6b53ce | explain a failed turn at its end | add the refusal/failure reason to the turn-end row, not just the prompt hand-back | in progress |
| dsh-android 4a875c0 | accept a queue item's content at the top level | ward's parser is fed nested items only; add the top-level branch and a test | in progress |
| dsh-android 4b05e19 | let a slow session list finish on its own door | ward's list is one CBOR call; add a read timeout and a door-fault rethrow | in progress |
| dsh-android 6b43c09 | read the upgraded session-list array | ward's bridge normalises shape already; add a test for the bare array | in progress |
| dsh-android e0f5786 | no RPC deadline for /compact | ward already uses a 20-minute CBOR deadline; record it as satisfied | done |
| dsh-android 07b86d8 | compact waits for its summary | ward has no client deadline; record it as satisfied | done |

## Work that needs the daemon or protocol first

| upstream | what it does | ward prerequisite | state |
|---|---|---|---|
| dsh-linux f009200, 3c0f0f2, 89a8c1a, cd78395; dsh-android estate and tenant clusters | estate/model-load chips, tenant cards, load progress, router tab | `estate.status` now serves demesne's live `/status` as `{json}`; the Android chips/cards and the desktop local-engine chip are ported. Outstanding: wiring the Android Router tab to the estate document (its host renders empty under test, so the terminal readout stays), and the DSH metrics view, which the payload does not carry | partially done |
| dsh-linux 96da4a4 queue steer | addressable queued messages | `session.queue` items now carry stable ids, with `session.queue.remove` and `session.queue.steer` | done |
| dsh-linux 105bb10 picker create-folder | create a directory from the picker | `host.mkdir` in core and the daemon, with the picker calling it and refreshing; containment checks the parent directory | done |
| dsh-linux d7b6d30, 56b548d | one-stream terminal attach, uplink, honest failures, title | `terminal.attach` replies with a 64 KiB ring snapshot before live output; `terminal.list` and `terminal.title` exist, and write/attach/resize answer `not_found` honestly | done |
| dsh-linux 4dc7504, 7e2e09d, ed3b7c0, 34fde91 | agent presets: read, choose, strip | an agent-preset concept in ward (session preset: system prompt, model defaults) plus protocol and storage | daemon and protocol done: `preset.save/list/get/delete/apply`, stored in `<state>/presets.cbor`; clients still open |
| dsh-linux 788246f, 176f55d; dsh-android e975536 | attachment references, image content; DSH 0.2.2 schedule verbs | attachment references carry a SHA-256 and a sniffed media type in the durable event and in `attachment.list`; `schedule.update` exists. Provider image parts are still open | partially done: references done, image parts open |
| dsh-linux c581446, 05cd5fa | schedule edit and target | `schedule.update` in core and protocol | done |
| dsh-linux 5776d71 | workspaceFiles/settings renamed calls | the desktop FilesDock now shows a workspace listing from `session.file_list` (cwd plus one level, sizes, best-effort mtime, refresh, open); `settings.get/set` already exist | done (desktop view) |

## Not applicable, with the reason kept on the record

- `1bd8586`, `cabfa95`, `3d92cff`: documentation of the DSH host contract, no
  ward behaviour to port.
- The DSH `session/attachment` and `directoryPicker/list` protocol shapes and the
  web UI are DSH's own contracts; ward's equivalents are `attachment.*`,
  `host.list` and its own clients.
