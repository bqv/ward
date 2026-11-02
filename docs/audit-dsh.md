# What DSH does that ward does not

`docs/parity.md` audits ward against its own `README.md`. This audits it against
**the harness it is modelled on** — the packages under
`/usr/lib64/node_modules/@deepseek-ai/dsh/node_modules/@deepseek-ai/`. Their
names are the evidence for what DSH treats as a first-class behaviour; where I
have not read the package the row says so rather than guessing at semantics.

## Policies ward has no equivalent for

| DSH package | what it implies | ward today |
|---|---|---|
| `fs-observation-policy` | read a file before overwriting it; a write to something never read is refused or escalated | **absent**. ward's `policy` module is a per-tool allow/ask/deny table (`read`/`grep` allow, `write`/`edit`/`exec` ask) and tracks no paths. A `write` to a file the session never read is allowed once approved. |
| `sandbox-policy`, `sandbox`, `sandbox-local`, `bash-sandbox`, `fs-sandbox` | a confinement layer: workspace-write, read-only paths, blocked escalation | **absent as policy**; ward relies on the OS (the cgroup) and on `wd_proc_run`'s child. There is no per-session file sandbox inside ward. |
| `permission-presets`, `user-approval` | named approval presets (ask-everything, ask-writes, allow-all) rather than a fixed table | **partial**: approvals exist (core state, keyed by call and epoch, first answer wins), but the policy is hard-coded in the module, not preset-driven. |
| `tool-call-timeout-policy` | per-tool time limits with a defined outcome | **partial**: `wd_http_*` and `wd_proc_run` carry timeouts, but there is no policy layer deciding what a timed-out tool means. |
| `spill-policy` | large tool output spills to a file instead of filling the context | **absent**: `WD_TOOL_MAX` truncates and the truncation is silent. |
| `compaction-tool-result-pruner` | old tool results are pruned before history is compacted | **absent**: `session.compact` summarises, it does not prune tool results first. |
| `atomic-write` | a write lands whole or not at all (temp file + rename, mode preserved) | **unknown in ward**: the tools module writes directly; I have not verified whether it renames. Worth checking before anything else here. |
| `session-checkpoint-policy` | when a checkpoint is taken automatically | **partial**: checkpoints exist as explicit calls; nothing decides when to take one. |
| `repeat-tool-reminder` | notice when the same tool call repeats and nudge | **absent**. |
| `tool-working-directory` | a per-session working directory with rules for paths outside it | **partial**: sessions carry a `cwd`, but path checks against it are the caller's problem. |
| `client-ui-secret-mask`, `secret` handling | secrets masked in events and UI | **partial**: credentials live in a 0600 file and never enter events, but nothing scans event payloads for a secret that leaked into one. |

## Behaviours ward has, for comparison

Approvals (core state, first answer wins, survives reload), the frozen prompt
with `AGENTS.md` rules, skills and slash commands, web search as a skill over a
core capability, MCP over a child process, PTY, attachments, scheduler, goals,
compaction, subagents, module reload and migration, the code index, the QUIC
device path, and now the cgroup and the cross-process device lock.

## The order I would close them in

1. **`fs-observation-policy`** — the one with a real failure behind it: an agent
   emptying a file it never read. Small, testable, and it belongs in core
   (it is a durable invariant per session, not a judgement).
2. **`atomic-write`** — first verify what the tools module does today; if it
   writes in place, fix it before anything else, because that is the same class
   of damage.
3. **`spill-policy`** — silent truncation is a lie about what a tool returned.
4. **permission presets** — the approval table becomes configuration rather than
   code, which is what makes 1 and 2 adjustable per project.
5. **the rest** (pruner, checkpoint policy, repeat reminder, secret masking):
   each is a small module or a policy hook, none blocks the others.

Note on scope: this document is an audit, not a plan the README approves.
`README.md`'s parity table is the scope contract, and none of these rows is in
it — so implementing them is a scope decision for the owner, not a gap ward is
failing to meet.
