# The user journey, and what is wrong with it

A review of what it is actually like to use this branch, written by walking the journey
surface by surface rather than from memory of having built it. It is deliberately
unflattering: the parts that work are documented elsewhere, and this is for the parts
that do not.

Some of it has since been fixed. Where that is true it says so, and says what the fix
was, because a review that quietly edits itself into being right is worth nothing.

## The journey as it was

**Getting in.** You build a fork of Godot, open your project, and nothing tells you
anything is different. The module creates **seven** user-visible surfaces — a chat dock,
an Agent Activity bottom panel, an Agent Terminal bottom panel, a GodotAI main-screen
workspace, and three modal dialogs — and there is no first-run path through them. There
are three ways to connect a model (the in-editor terminal running an agent, an external
client over the relay, the chat dock borrowing a connected client's model by sampling)
and no opinion about which is the default. A new user's first question is "what can this
do?" and nothing answers it.

**Asking for something.** You type prose into a terminal at the bottom of a *visual*
editor. You have a node selected in the scene tree and you still have to describe it in
words. The editor knows what you are looking at, what is selected, which scene is open,
what just errored — and none of it reached the model unless the model went and asked, in
a separate round trip, having first guessed that it should.

**The model choosing what to do.** `tools/list` returns 95 tools. The skills are the
intended path — that is the whole point of the note in `NEXT.md` about the tool surface
being a reliability risk — but discovery presents `Godot_ManageNode` and
`playtest-a-goal` with equal prominence, so the primitives win by numeric mass. The risk
was written down and then the surface that causes it was built anyway.

**Watching it work.** The worst part, and structural rather than cosmetic. Activity and
Terminal are both bottom-panel items and Godot shows one of those at a time. So while
the agent talks to you in the terminal, the panel showing what it is *doing* is hidden
behind it. The observability stream, deliberately built as a stream rather than a panel,
is in practice the tab you are not looking at.

**Being interrupted.** Approvals and proposals are both modal. Modal is right for
"irreversible, decide now" and wrong for everything else, so a long autonomous run is a
sequence of blocking popups. The user asked for autonomy and the interface asks for
attention.

**Seeing evidence.** Screenshots exist, viewport capture exists, the playtest report is
good. They arrive as JSON in a terminal. For a visual medium the primary channel is text.

**Undoing.** Checkpoints are per-tool-call. There is no "undo what the agent just did"
as one gesture over a task: a twelve-call session that went wrong is unwound twelve
times, by hand, from a list.

**Coming back tomorrow.** Nothing persists. `.agent/` is excellent memory — for the
tooling project. The game being edited had none, so every session relearned the
codebase, the conventions, and the thing that broke last time.

## Seven criticisms

1. **Nobody has made a game with it.** Every verification is a script we wrote. The
   benchmarks exist and have never been run against a real model. This is the biggest
   hole: the work may be optimising for what is verifiable rather than what is useful,
   and there is currently no way to notice.

2. **The centre of gravity is outside the editor.** The user lives in a terminal; the
   editor is a thing being driven. Seven scattered surfaces are the symptom — nobody
   designed a place to *be*, because the place to be was always somewhere else.

3. **Tools are engine operations, not design intentions.** `Godot_ManageNode create` is
   a verb the engine has. "Make the enemy come from the left" is what someone wants.
   Every session re-bridges that gap from scratch and gets it differently wrong.

4. **No project memory.** The highest-leverage missing thing, and the cheapest: a file
   format and a tool, not an architecture.

5. **Vision is an afterthought.** No before-and-after, no visual diff, no filmstrip of a
   playtest. The viewport can be captured and the image is treated as an attachment
   rather than as the evidence.

6. **The agent cannot explore the editor.** 807 documented classes sat in the tree
   unread while the model worked from a remembered API — on a 4.8-dev fork, and with no
   way at all to know the project's own classes.

7. **The loop is single-player.** Two game processes embed simultaneously (DEC-0011
   measured it) and the only thing using more than one is variant comparison. Parallel
   exploration — one goal, three approaches, compared — is sitting there unbuilt.

## What has been done since

| Criticism | Status |
|---|---|
| 4 — no project memory | **Fixed.** `res://.godot_ai/memory/*.md`, recalled as an index rather than dumped, bounded, and names slugified so traversal is unrepresentable. |
| 6 — cannot explore the editor | **Fixed.** `Godot_LookupClass` reads the class reference, the project's own script classes included, headless and under a display alike. |
| Selection is not the prompt | **Fixed.** `Godot_GetEditorStatus` reports the selection, the current workspace and the open script, so context arrives without being asked for. |
| Panels hiding each other | **Mitigated.** The terminal carries a one-line summary of the agent's latest operation and a button that brings the full stream forward. Moving a panel was tried before and reverted — in a side dock it shrank the Inspector until a capture tool broke. |
| Headless is second-class | **Fixed.** See `godot-ai-headless.md`: a per-capability policy in the environment, and the two human-answer tools no longer wait five minutes for a dialog nobody can see. |
| 1 — nobody has made a game with it | **Open, and still the biggest one.** |
| 2, 3, 5, 7 | **Open.** |

## The flows worth changing next

- **Propose-and-diff as the default mode, not a tool.** `Godot_ProposeChange` exists and
  is good; it is opt-in. Flip it: the agent's normal output is a plan shown *in the
  scene* — ghosted nodes, highlighted properties, affected files marked in the
  FileSystem dock — accepted there rather than in a modal listing paths.
- **A task-level undo.** Group checkpoints by task and offer one revert. The safety net
  exists but you have to weave it while falling.
- **Ambient findings.** Read-only observations volunteered without being asked: an
  orphaned node, a signal going nowhere, a playtest 12% slower than the last. Non-modal,
  dismissible. This is what would make it an editor with an AI in it rather than a
  terminal that happens to be inside Godot.
- **Skills-first discovery.** Make the higher-level jobs the prominent surface and the
  95 primitives the thing they compose, rather than presenting both equally in every
  interaction.
- **One surface, three planes.** The workspace specification already says this — dock is
  the control plane, main screen the view plane, Activity the evidence plane — and the
  implementation did not follow it.

## On "filling out the visuals"

The phrase reads two ways and they are different products.

**A: the tool becomes visual.** Visual diffs, filmstrips of a playtest, a plan drawn in
the scene, evidence you look at instead of parse. This is squarely in scope and it is
what criticism 5 is about.

**B: the agent generates game art.** Sprites, textures, models.

B needs asset pipelines, licensing answers and vendor image APIs inside an editor that
deliberately holds no credentials, and it would dilute the one capability nothing else
has: observing, operating and modifying a *running* game. There are many products doing
B and none doing the closed loop. The recommendation is to build A hard and treat B as
out of scope for this branch — recorded here as a recommendation rather than a decision,
because it is not one this document gets to make.
