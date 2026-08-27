#!/usr/bin/env python3
"""Scoring a benchmark run: did it work, what else did it touch, and can anyone check.

Pure. No editor, no network, no model. Everything here reads files that exist after a
run and decides what they say, which is what makes the score reproducible and what makes
these functions testable without any of the machinery they are scoring.

Three numbers per task, deliberately not collapsed into one:

  solved      - the task's own oracle held.
  collateral  - files changed that the task did not license. An agent that fixes the bug
                and quietly rewrites three other scripts has not done the job, and a
                single score hides exactly that.
  evidence    - the agent left something a person can check: a playtest report, a
                recorded session, a checkpoint. An answer with no evidence is a claim.
"""
import hashlib
import json
import os
import re


# --- reading a project -------------------------------------------------------

def file_digests(root):
    """Every file under `root`, as {relative path: sha256}.

    Skips `.godot`, which the engine rewrites on every open and which no agent is
    responsible for.
    """
    digests = {}
    for directory, subdirectories, names in os.walk(root):
        subdirectories[:] = [d for d in subdirectories if d != ".godot"]
        for name in names:
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            with open(path, "rb") as handle:
                digests[relative] = hashlib.sha256(handle.read()).hexdigest()
    return digests


def changed_files(before, after):
    """Paths that differ between two digest maps, including added and removed ones."""
    changed = set()
    for path, digest in after.items():
        if before.get(path) != digest:
            changed.add(path)
    for path in before:
        if path not in after:
            changed.add(path)
    return sorted(changed)


def collateral_changes(before, after, licensed_paths):
    """Changed files the task did not license.

    The licence is a list of exact paths rather than a pattern: "it was allowed to touch
    the scripts folder" is how an agent ends up allowed to touch everything.
    """
    licensed = set(licensed_paths)
    return [path for path in changed_files(before, after) if path not in licensed]


def read_text(root, relative):
    path = os.path.join(root, relative)
    if not os.path.exists(path):
        return None
    with open(path) as handle:
        return handle.read()


# --- oracles -----------------------------------------------------------------
#
# Each returns (held, detail). The detail is what a person reads when a task fails, so
# it says what was looked for and what was found instead - never just "false".

def oracle_number_at_least(root, relative, pattern, minimum):
    """A number matched by `pattern` in a file is at least `minimum`.

    A range rather than an exact value on purpose. The task is "make the jump clear the
    gap", not "write 220": an agent that reasons its way to 210 has solved it, and an
    oracle that demands one number is testing obedience instead.
    """
    text = read_text(root, relative)
    if text is None:
        return False, "%s does not exist" % relative
    found = re.search(pattern, text)
    if not found:
        return False, "no number matching %s in %s" % (pattern, relative)
    try:
        value = float(found.group(1))
    except (IndexError, ValueError):
        return False, "the match in %s is not a number: %r" % (relative, found.group(0))
    if value < minimum:
        return False, "%s is %g, which is below the %g needed" % (relative, value, minimum)
    return True, "%s is %g" % (relative, value)


def oracle_text_present(root, relative, needle):
    text = read_text(root, relative)
    if text is None:
        return False, "%s does not exist" % relative
    if needle not in text:
        return False, "%s does not contain %r" % (relative, needle)
    return True, "%s contains %r" % (relative, needle)


def oracle_text_absent(root, relative, needle):
    text = read_text(root, relative)
    if text is None:
        return False, "%s does not exist" % relative
    if needle in text:
        return False, "%s still contains %r" % (relative, needle)
    return True, "%s no longer contains %r" % (relative, needle)


def oracle_all(root, oracles):
    """Every oracle in turn; the first failure is the answer.

    Reporting the first failure rather than all of them is deliberate: the later ones
    usually fail because of the first, and a list of five failures reads as five
    problems.
    """
    for oracle in oracles:
        held, detail = oracle(root)
        if not held:
            return False, detail
    return True, "every check held"


# --- evidence ----------------------------------------------------------------

def evidence_found(user_data_root):
    """What the agent left behind that a person could check.

    Looks where the tools write: playtest reports, recorded sessions, checkpoints. An
    empty result is not a failure of the task, but it is worth knowing - an answer with
    no evidence is a claim, and the difference between the two is most of what this
    fork is for.
    """
    found = {"playtests": [], "sessions": [], "checkpoints": 0}
    if not user_data_root or not os.path.isdir(user_data_root):
        return found

    playtests = os.path.join(user_data_root, "godot_ai_playtests")
    if os.path.isdir(playtests):
        for entry in sorted(os.listdir(playtests)):
            report = os.path.join(playtests, entry, "report.json")
            if os.path.exists(report):
                try:
                    with open(report) as handle:
                        data = json.load(handle)
                except (OSError, ValueError):
                    continue
                found["playtests"].append({
                    "slug": entry,
                    "verdict": data.get("verdict"),
                    "goal": data.get("goal"),
                })

    sessions = os.path.join(user_data_root, "godot_ai_sessions")
    if os.path.isdir(sessions):
        found["sessions"] = sorted(
            entry for entry in os.listdir(sessions)
            if os.path.exists(os.path.join(sessions, entry, "meta.json")))

    checkpoints = os.path.join(user_data_root, "godot_ai_checkpoints")
    if os.path.isdir(checkpoints):
        found["checkpoints"] = len(os.listdir(checkpoints))

    return found


# --- the scorecard -----------------------------------------------------------

def score_task(task, root, before_digests, user_data_root=None):
    """One task's result. `before_digests` is the project as it was handed over."""
    held, detail = task.oracle(root)
    after = file_digests(root)
    collateral = collateral_changes(before_digests, after, task.licensed_paths)

    return {
        "task": task.identifier,
        "category": task.category,
        "solved": held,
        "detail": detail,
        "changed": changed_files(before_digests, after),
        "collateral": collateral,
        "clean": held and not collateral,
        "evidence": evidence_found(user_data_root),
        # Filled in by a real run against a real model. Left absent rather than zero:
        # a zero here would read as "free and instant", which is a claim about a run
        # that has not happened.
        "seconds": None,
        "cost": None,
    }


def summarise(results):
    """The scorecard over several tasks.

    `clean` is reported beside `solved` because they are different claims and the gap
    between them is the interesting one: an agent that solves eight of ten but leaves
    collateral in six is not an eighty-percent agent.
    """
    total = len(results)
    solved = sum(1 for result in results if result["solved"])
    clean = sum(1 for result in results if result["clean"])
    with_evidence = sum(
        1 for result in results
        if result["evidence"]["playtests"] or result["evidence"]["sessions"])

    by_category = {}
    for result in results:
        bucket = by_category.setdefault(result["category"], {"total": 0, "solved": 0})
        bucket["total"] += 1
        bucket["solved"] += 1 if result["solved"] else 0

    return {
        "tasks": total,
        "solved": solved,
        "clean": clean,
        "with_evidence": with_evidence,
        "by_category": by_category,
        "results": results,
    }


def format_scorecard(summary):
    lines = []
    lines.append("%d/%d solved, %d of those without collateral changes, %d left evidence"
                 % (summary["solved"], summary["tasks"], summary["clean"],
                    summary["with_evidence"]))
    lines.append("")
    for result in summary["results"]:
        mark = "PASS" if result["clean"] else ("PARTIAL" if result["solved"] else "FAIL")
        lines.append("%-8s %-22s %s" % (mark, result["task"], result["detail"]))
        if result["collateral"]:
            lines.append("         also changed, unlicensed: %s"
                         % ", ".join(result["collateral"]))
    return "\n".join(lines)
