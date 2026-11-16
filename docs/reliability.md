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
| observed file state: read before overwrite, version-guarded write | `dsh-fs-observation-policy` | absent: a write to a file the session never read is allowed once approved; core tracks no paths | a turn writes a file it did not read and the policy refuses with a named reason; after a read it succeeds |
| tool-result retention: large output spills to a recoverable path with a notice, instead of being cut | `dsh-spill-policy`, `dsh-output-retention` | **started** `2e12095`: `fs.read` and `exec` capture up to 4 MiB, spill past 64 KiB to `<session>/spill/`, give the model a 256-byte head plus a notice naming the path, and put `truncated`/`spill`/`spill_size` on the `tool.result` event. Still absent: grep and search return their full output, a latent turn-breaker because the driver guest reads the model-visible text into a 512-byte buffer | an exec or read over 64 KiB returns a spill path, the file holds every byte, the model's text names the path; a grep over the same size does not break the turn |
| provider retry: 429/5xx/network retried with backoff | `dsh-llm-retry` | absent: the turn fails on the first non-2xx | a fixture answering 429 then 200 completes the turn; the retry count is visible in the event log |
| per-tool timeout policy with a defined outcome | `dsh-tool-call-timeout-policy` | partial: http and process calls carry timeouts, but no policy decides what a timed-out tool means and no `TOOL_TIMEOUT` outcome exists | a tool that exceeds its budget yields a timeout result the model can act on, not a stalled turn |
| compaction pruner: old tool results pruned before summarising | `dsh-compaction-tool-result-pruner` | absent | compaction of a history with large tool results keeps the head and tail and drops the middle with a counted notice |
| checkpoint policy: checkpoints before model requests and side effects | `dsh-session-checkpoint-policy` | partial: checkpoints are explicit calls only | a write/edit takes a checkpoint first, and a failed turn leaves one to return to |
| repeat-tool reminder: an advisory when the same call repeats | `dsh-repeat-tool-reminder` | absent | a third identical call in a turn emits a reminder event, and the call still runs |
| secret masking in events and UI | `dsh-client-ui-secret-mask` | partial: credentials never enter events by construction, but nothing scans a payload that leaks one | a fixture tool result containing a credential-shaped string is masked in the event and the transcript |
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
