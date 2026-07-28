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

Before context compaction or session completion, persist all active goals, failures,
evidence, working-tree expectations, and the exact next action under `.agent/`. Do not
rely on the current conversation as project memory.
