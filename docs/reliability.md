# Reliability parity with DSH

`docs/audit-dsh.md` lists what the harness does that ward does not. This file is
the programme that closes the parts that make DSH *reliable* rather than merely
feature-rich, with the evidence and an acceptance line per item. The inventory
is the installed DSH 0.2.2-rc.1 set under
`/usr/lib64/node_modules/@deepseek-ai/dsh/node_modules/@deepseek-ai/`; a package
name and its own description are the evidence that DSH treats a behaviour as
first-class.

## The gap, as of this note

| DSH behaviour | DSH package | ward today | acceptance |
|---|---|---|---|
| atomic write: a write lands whole or not at all, mode preserved | `dsh-atomic-write` | **closed** `3bf67a9`: `fs.write`/`fs.edit` write a temp file, fsync, chmod from the old target, rename, fsync the directory; symlinks still refused | gate_tool writes an existing 0640 file and asserts the text and the mode |
| observed file state: read before overwrite, version-guarded write | `dsh-fs-observation-policy` | **closed** (this slice): each successful read marks its resolved path with a durable `session.observed {path, mtime_ms, size}`, replayed into the observed set on load; `fs.write` and `fs.edit` refuse an existing file the session never read, and refuse a file whose mtime or size no longer matches the read, with a named reason | gate_tool covers read-before-write; gate_fs reads a file, changes it externally, proves the write is refused and the external content survives, re-reads and writes successfully, and sees the checkpoint before the result |
| tool-result retention: large output spills to a recoverable path with a notice, instead of being cut | `dsh-spill-policy`, `dsh-output-retention` | **closed for every text-bearing tool**: `fs.read`, `exec`, `grep`, `recall.search`, `mcp.call`, `web.search` and both python paths write anything past the 256-byte visible window to `<session>/spill/`, hand the model a head plus a notice naming the path, and put `truncated`/`spill`/`spill_size` on the `tool.result` event. The 256-byte window is forced by the driver guest, which reads the model-visible text into a 512-byte buffer and ends the turn when it does not fit | a large read, exec or grep returns a spill path whose file holds every byte, and no tool result can exceed the driver window |
| provider retry: 429/5xx/network retried with backoff | `dsh-llm-retry` | **closed for 429 and 5xx** (this slice): the turn keeps its provider request, a 429 or 5xx schedules a retry after 300, 600 then 1,200 ms (at most three), emits `turn.retry {turn, status, delay_ms}`, and the daemon restarts the same request from its saved URL and body. Not retried: a transport failure before the response headers | gate_tool answers 429 once from the fixture, sees `turn.retry` with status 429 and then the retried 200 completing the turn. Pre-header transport failure is still open: the job child writes a four-byte zero marker when curl fails before headers, but with a dropped connection the daemon never observes completion (no http_done, no run_after failure), so the turn waits and no retry is emitted; a dedicated `gate_net` reproduced the wait. The next attempt must find why the job fd produces no completion event for that case |
| per-tool timeout policy with a defined outcome | `dsh-tool-call-timeout-policy` | **closed** (this slice): every process-backed tool returns `{ok:false, timed_out:true, error:"… exceeded its N ms timeout"}` instead of a generic failure, and `exec.run` — which is submitted asynchronously — now emits its `tool.result` event at all, carrying `timed_out`, `truncated`, `spill` and `spill_size`; the synchronous `tool.result` path carries `timed_out` too | gate_tool runs `/run tail -f /dev/null`, approves it and sees a `tool.result` with `timed_out` true after the 10 s budget, with the turn still ending normally |
| compaction pruner: old tool results pruned before summarising | `dsh-compaction-tool-result-pruner` | absent | compaction of a history with large tool results keeps the head and tail and drops the middle with a counted notice |
| checkpoint policy: checkpoints before model requests and side effects | `dsh-session-checkpoint-policy` | **closed for side effects** (this slice): before an `fs.write` or `fs.edit` capability runs, the turn emits a durable `session.checkpoint` labelled "before a side effect" through the event callback, so it is both logged and broadcast. Not done: an automatic checkpoint before every model request | gate_tool approves a write, sees `session.checkpoint` with that label before the `tool.result`, then the write lands |
| repeat-tool reminder: an advisory when the same call repeats | `dsh-repeat-tool-reminder` | absent | a third identical call in a turn emits a reminder event, and the call still runs |
| secret masking in events and UI | `dsh-client-ui-secret-mask` | **closed for event and transcript masking** (this slice): every turn event is walked before it is logged or broadcast and credential-shaped runs (`sk-`, `ghp_`, `AKIA`, `Bearer ` followed by a token) become `[redacted]`; the live ring buffer carries the masked text as well, and failure messages are masked too | gate_fs reads a file containing a fake `sk-` and `ghp_` token, sees every event of the turn free of the raw value and at least one `[redacted]` |
| ask the human mid-turn | `dsh-user-questions`, `dsh-tool-ask-user` | absent: approvals only | a turn asks a free-form question and resumes with the answer |
| todo list as a tool | `dsh-tool-todo` | absent: goals exist, no per-turn todo | the model writes a todo and the client shows it without a renderer change |

## Order and reason

1. **Atomic writes** — done; it was the same damage class as the fs-observation
   incident and needed no policy seam.
2. **Spill and output retention** — silent truncation is a lie about what a tool
   returned, and every later judgement (the model's and the reader's) inherits
   it. Bounded, testable in core plus one fixture.
3. **Observed file state** — the failure that motivated the audit: an agent
   emptied a file it had never read. It needs one core fact (the observed set
   per session) and one policy rule, so it follows the seam that already exists.
4. **Provider retry** — a transient 429 must not end a turn; the retry budget
   and the event are the contract.
5. **Per-tool timeout policy** — turn a hung tool into a result.
6. **Compaction pruner, checkpoint policy, repeat reminder** — long-session
   quality; each is a small module or hook and none blocks the others.
7. **Ask-user and todo** — feature parity after the reliability floor, not
   before it.

## What is deliberately not copied

DSH's protocol shapes (session list arrays, schedule/edit, attachment
references), its web UI, its client packages and its agent presets are DSH's
own contracts, not behaviour ward needs; `docs/protocol.md` is ward's frozen
surface and `README.md`'s parity table is its scope contract. `audit-dsh.md`
remains the wider audit; this file only covers the reliability programme the
owner approved on 2026-11-16.
