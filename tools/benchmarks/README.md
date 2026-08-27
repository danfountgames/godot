# Benchmarks

Counting tools, tests and verified requirements measures engineering coverage. It says
nothing about whether an agent is any good at this, and those are different questions:
a fork can have every runtime primitive and still be wrong about the game half the time.

This measures the second question. It is deliberately the part that is hard to copy —
another project can add runtime tools in a month; a calibrated benchmark suite with
honest oracles takes much longer, and it is what tells you whether a change to a prompt,
a tool description or a model made things better or worse.

## What is here

| File | What it is |
|---|---|
| `tasks.py` | The task definitions: a prompt, a category, and an oracle that needs no model to evaluate |
| `projects.py` | Builds each benchmark project, with its defects deliberately planted |
| `scoring.py` | Evaluates oracles and assembles a scorecard. Pure; no editor, no network |
| `run_selfcheck.py` | Proves the benchmark measures something: every planted defect present, every oracle failing before the fix and passing after |
| `tests/run_tests.py` | Unit tests for the scoring logic |

## How a run works

1. Build a fresh copy of a benchmark project (`projects.build`).
2. Give an agent the task's prompt and the project, through the ordinary product path —
   the relay, the editor, the tools. Nothing here talks to a model; that is the point.
   The harness does not care how the agent was driven.
3. Score the resulting project with `scoring.score_task`, which reads the files and the
   reports the agent left behind.

The score is not a single number. Each task reports:

- **solved** — the oracle for the task itself held.
- **collateral** — files the agent changed that the task did not license it to change.
  This is the measurement worth having and the one nobody publishes: an agent that fixes
  the bug and quietly rewrites three other scripts has not done the job.
- **evidence** — whether the agent left a report, a session or a checkpoint that a
  person could check. An answer with no evidence is a claim.

## Why the self-check exists

A benchmark whose planted defect does not actually reproduce measures nothing, and it
fails silently in the flattering direction: every agent "passes". `run_selfcheck.py`
builds each project twice — once as shipped and once with the known fix applied — and
requires every oracle to fail on the first and pass on the second. An oracle that passes
on the broken project is a broken oracle, and the self-check says so by name.

Run it whenever a task or a project changes:

```sh
python3 tools/benchmarks/run_selfcheck.py
python3 tools/benchmarks/tests/run_tests.py
```

## What is not here yet

Timing and model cost. Both need a real run against a real model, and recording a number
from a run that has not happened would be worse than leaving the column empty. The
scorecard has the fields; nothing fills them.
