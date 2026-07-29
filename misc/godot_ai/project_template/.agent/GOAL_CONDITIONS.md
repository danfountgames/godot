# GOAL_CONDITIONS

Ready-to-paste `/goal` conditions for this project, in the order you would use them.

Read *Running under `/goal` and `/loop`* in `AGENTS.md` first. The short version: the
evaluator is a small fast model that sees only the conversation, so every condition here
names something the agent **prints**, never something it would have to open a file to
check.

Each condition assumes the agent ends every turn with the `GODOT-AGENT-STATUS` block that
`AGENTS.md` specifies. Without that block none of these work.

---

## 1. Bootstrap — prove the interface before building on it

Use this first, once. It is short and it fails fast, which is what you want from a
health check.

```
/goal a GODOT-AGENT-STATUS block has been printed showing editor: up, and .agent/TOOLING.md
has been filled in from calls actually made this session — including whether a display is
available, whether real input reaches the running game, and which test command works. Stop
after 8 turns.
```

## 2. First playable slice

```
/goal the most recent GODOT-AGENT-STATUS shows tree: clean, a commit sha, real_input:
pass for a route that launches the game and performs the core action from the spec, and at
least one screenshot of that action has been captured and inspected this session. Stop
after 25 turns.
```

## 3. The whole game — the main goal

```
/goal the transcript's most recent GODOT-AGENT-STATUS block shows DONE-CHECK: DONE with all
seven Definition-of-Done clauses marked PASS, tree: clean, issues_open: 0, and a commit sha;
and the turn that printed it also showed the real-input route passing. If a turn reports the
same `next` value three times running, stop and report the obstacle instead. Stop after 60
turns regardless.
```

## 4. Closing a specific defect list

```
/goal every issue in .agent/ISSUES.md that was open at the start of this goal has been
either closed with a named regression check or explicitly recorded as won't-fix with a
reason, and each closure was reported in a GODOT-AGENT-STATUS block with issues_open
decreasing. Stop after 30 turns.
```

## 5. Release verification

Only after the game itself is done.

```
/goal a web export of this project has been produced, served locally, loaded in a browser,
and the principal player route performed against it with real input; the status block
reports the export path and the route result, and a screenshot of the running export has
been captured and inspected. Stop after 20 turns.
```

---

## Writing your own

- **Name the artifact, not the feeling.** "DONE-CHECK: DONE" is checkable; "the game is
  polished" is not.
- **Require the evidence in the same turn.** Otherwise a pass from twenty turns ago can be
  quoted back at the evaluator.
- **Include an anti-thrash clause.** Repeating the same `next` three times means stuck, and
  stuck should stop rather than burn turns.
- **Cap the turns.** Every condition here does. A goal with no bound is a goal that can
  cost whatever it likes.
- **Do not restate the Definition of Done.** It has a 4,000-character limit, and two copies
  drift apart.

## Anti-patterns

| Condition | Why it fails |
|---|---|
| `/goal the game is finished` | Nothing measurable; judged on prose. |
| `/goal all tests in .agent/TEST_MATRIX.md pass` | The evaluator cannot read that file. |
| `/goal make the game fun` | Not decidable by any evaluator, including a human. |
| `/goal DONE-CHECK: DONE` | No turn cap, no anti-thrash, no requirement that evidence be fresh. |
| `/goal ...` with no status block in `AGENTS.md` | The evaluator has nothing to read; it will guess. |
