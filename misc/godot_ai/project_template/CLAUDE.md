@AGENTS.md

# Claude Code adapter

The complete project instructions are imported from `AGENTS.md` above. That file is
the single source of truth; this file adds only what is specific to Claude Code.

Use subagents for the independent Builder, Test Engineer, Visual Critic, Black-box
Playtester, Adversarial QA, and architecture-review roles described there whenever
subagents are available. Give each one its own evidence to produce, and do not ask one
reviewer merely to confirm another — agent consensus is not proof.

When subagents are not available, run clearly separated review passes with separate
evidence, and record in `.agent/PLAYTEST_LOG.md` or `.agent/VISUAL_LOG.md` that
independence was reduced. Do not present a single reasoning pass as several agents.

## Unattended runs

`/goal` and `/loop` are the two ways to run this project without a human prompting each
step. Read *Running under `/goal` and `/loop`* in `AGENTS.md` before using either, and
take the condition from `.agent/GOAL_CONDITIONS.md` rather than inventing one.

The single thing that matters: **print the `GODOT-AGENT-STATUS` block at the end of every
turn.** A `/goal` evaluator is a small fast model that reads only the conversation — it
cannot run a command or open a file, so anything not in that block is invisible to it, and
a goal judged on prose alone will end this run early.

Pair either with auto mode. Neither grants permissions, and an unattended run stalls at
the first permission prompt.

Before context compaction or session completion, persist all active goals, failures,
evidence, working-tree expectations, and the exact next action under `.agent/`. Do not
rely on the current conversation as project memory.
