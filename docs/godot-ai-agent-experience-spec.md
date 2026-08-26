# Godot AI — agent experience specification

Third specification in the set, and the first one about the *human* rather than the
protocol.

- `docs/godot-ai-clone-spec.md` defines the product: an MCP server in the editor, a
  relay, permissions, checkpoints, skills. Delivered; 53 of 55 requirements verified.
- `docs/godot-ai-agent-interface-spec.md` defines the capabilities an agent needs to
  build a game through the product rather than around it. Delivered; 41 of 41 verified.
- **This document** defines what the two of them still do not give anyone: a way to
  *see* the agent work, and workflows that compose the primitives into something a
  person would recognise as a task.

## Why this exists

The fork currently exposes 72 tools. An agent can play the game, read its live scene
tree, inject real input at four levels, edit properties mid-flight, capture frames,
and export a full profiler window. Every call is capability-gated, checkpointed and
audited.

None of that is visible. A person watching the editor sees a chat panel and a scene
that changed. The audit log knows exactly what happened and no interface reads it back.
The primitives are also uncomposed: "play the game and tell me if it still works" is
nine tool calls and a loop the client has to invent every time, differently.

So the two problems this document addresses are **legibility** and **composition**, and
they are related. A composed workflow that runs for four minutes unattended is only
tolerable if you can watch it. An activity view is only worth building if there is
sustained activity to watch.

### Non-goals

- **Asset generation.** Not in scope, now or later. GDC's 2026 survey puts developer
  sentiment against generative AI at 52% negative, concentrated hardest in art and
  design, while actual usage clusters in prototyping, coding and research. The fork's
  advantage is the runtime layer, and the runtime layer is a verification story.
- **A vendor model in the editor.** Unchanged from the original spec: the chat dock
  borrows the connected client's model through MCP sampling. Nothing here adds a
  credential.
- **Shipping any of this in an exported game.** `can_build` already refuses non-editor
  builds and a template build proves the symbols are absent. Everything below is
  editor-only, and the runtime half rides the existing debugger channel.

## Vocabulary

| Term | Meaning |
|---|---|
| **Session** | One recorded or replayed run of the game, identified by a slug, stored under `user://godot_ai_sessions/`. |
| **Trace** | The input stream of a session, frame-locked: one record per frame that carried an event. |
| **Assertion** | A named condition over runtime state, captured during recording, checked during replay. |
| **Playtest** | An agent-driven session with a goal, a budget and an oracle, rather than a recorded trace. |
| **Activity** | The stream of tool calls the service has executed, as the dock renders it. |

---

## E — Activity and legibility

The editor must show what the agent is doing while it does it.

| ID | Requirement | Rationale |
|---|---|---|
| E1 | The service publishes a live activity stream: one record per tool call, carrying name, capability, a one-line description from `describe_invocation`, start time, duration, outcome, and the checkpoint id if one was taken. | The audit log already records all of this. It is written to disk and read by nobody. |
| E2 | An Activity dock renders that stream live, newest last, with the in-flight call distinguished from finished ones. | The minimum bar for "watch it work". |
| E3 | Each record names the concrete subjects it touched — scene node paths and `res://` file paths — extracted from the tool's own arguments, not guessed. | "Ran `Godot_ManageNode`" is not watchable. "Renamed `Main/Player/Sprite`" is. |
| E4 | Selecting a record reveals its subjects in the editor: the node in the Scene dock, the file in the FileSystem dock. | Presence, not just logging. This is the collaborator-cursor effect. |
| E5 | The dock renders the checkpoint timeline as a scrubber, and offers per-record "what changed" using the existing diff, with revert. | Checkpoints and `Godot_DiffCheckpoint` already exist; nothing surfaces them. |
| E6 | Activity survives a restart of the dock but not of the editor; the audit log remains the durable record. | Do not build a second persistence layer over the one that already works. |
| E7 | The stream is readable as a tool as well as a dock (`Godot_GetActivity`), so a headless run and the end-to-end script can assert on it. | Anything only reachable through a UI cannot be regression-tested here. |

## S — Sessions: record and replay

The capability that no authoring-only integration can copy, because it needs the
running game.

| ID | Requirement | Rationale |
|---|---|---|
| S1 | `Godot_RecordSession` starts recording the running game's input stream to a named session, and stops on request or on a frame budget. | An agent's play becomes a repeatable regression test with no test code written. |
| S1b | **Recording covers agent-injected input only.** Observing a *human* playing the game window needs a hook in the game's input pipeline, which does not exist. Until it does, no tool, description or document may imply a designer can author a trace by playing. | The existing trace is written by the four `_send_*` handlers in `mcp_runtime_agent.cpp` and sees nothing else. The "designer plays, we record" pitch is the better product and it is a *separate*, larger piece of work — writing it into S1 would have shipped a tool whose description was false. |
| S2 | The trace is frame-locked: each record carries the process frame it belongs to, not a wall-clock time. | Wall-clock replay is not reproducible. Frame numbers are the only stable index the engine offers both sides. |
| S3 | Assertions may be captured during a recording (`Godot_AssertRuntimeState`), each naming a node path, a property and the observed value, tagged with the frame. | The trace says what was done; assertions say what should have resulted. A trace without assertions only detects crashes. |
| S4 | `Godot_ReplaySession` launches the game, re-injects the trace frame by frame, evaluates each assertion at its frame, and reports the **first** divergence with the recorded value beside the observed one. | First divergence, because everything after it is downstream noise. |
| S5 | Replay accepts a speed multiplier, applied through the existing time-scale control, and reports the multiplier it actually achieved. | An eight-times replay that silently ran at one times is a lie about coverage. |
| S6 | Replay reports honestly when it cannot be deterministic: any frame where the engine's frame budget was missed badly enough that the trace and the game disagree on frame count is flagged, and the session is reported `indeterminate` rather than `passed`. | Determinism under physics and timing is the hard part. A harness that hides its own unreliability is worse than none. |
| S7 | `Godot_ListSessions` enumerates recorded sessions with their frame count, assertion count, and last replay verdict. | Discovery, same shape as `Godot_ListSceneTests`. |
| S8 | Traces are JSON Lines under `user://godot_ai_sessions/<slug>/`, in the same shape the profiler export already uses, with a reading guide in the tool reply. | One export convention, already proven against the 8 MiB debugger-channel limit. |
| S9 | A playtest that finds a crash emits a replay file for it. | The bug arrives reproducible or it arrives as an anecdote. |

## P — Playtest sessions

| ID | Requirement | Rationale |
|---|---|---|
| P1 | `Godot_StartPlaytest` takes a goal in prose, a frame or wall-clock budget, and an explicit stop oracle; it launches the game and returns a session handle immediately. | The client should not hold a socket open for four minutes. |
| P2 | The session loop runs in the editor, not the client: perceive (scene tree, errors, frame), decide (the client's model over MCP sampling), act (input injection), repeat. | The editor owns the game. A client-driven loop pays a round trip per frame. |
| P3 | `Godot_GetPlaytestReport` returns crashes with the preceding frame sequence and stack trace, profiler windows around each frame-time spike, scenes entered, and the goal verdict. | The report is the product; the play is the means. |
| P4 | A playtest is stoppable mid-run and reports partial results. | Four minutes is long enough that "cancel" must work, and the cancellation frame is already proven end to end. |
| P5 | Every input a playtest injects appears in the activity stream and the input trace. | An agent that plays unobserved is exactly the thing this document exists to prevent. |

## D — Design conversation

| ID | Requirement | Rationale |
|---|---|---|
| D1 | `Godot_ProposeChange` submits a plan — an ordered list of concrete edits, each with a description and the tool call that would perform it — and renders it in the dock as a checklist with per-item Apply and Reject. | Design work is proposed and chosen, not executed and reported. |
| D2 | Applying goes through `EditorUndoRedoManager` as one transaction per item, so a rejected item leaves no trace and an applied one is undoable. | Unchanged rule from the original spec; a plan does not get to bypass it. |
| D3 | `Godot_OfferVariants` produces several named versions of one change, each behind its own checkpoint, with a switcher that flips between them while the game runs. | Checkpoints already make variants free. Nothing exposes them as a design gesture. |
| D4 | A variant may be promoted to the persistent scene, or all of them discarded, in one call. | The gap between "tuned while playing" and "kept" is currently manual and lossy. |

---

## Sequencing

E before P before D, and S alongside P.

The Activity dock (E) is first because it is the surface the others are watched
through, and because it needs no new runtime capability — it reads a stream the service
can already produce. A playtest session (P) without E is a progress bar. A proposal
flow (D) without E has nowhere to render.

S and P share the session substrate, the report format and the storage layout, so they
are built together even though S is the smaller and more certain of the two.

## Verification rules specific to this tranche

Carried forward from the interface spec, which learned them the hard way:

- **A UI requirement is not verified by a unit test that calls the method the button
  calls.** It is verified by locating the control with `Godot_FindControl` and pressing
  it with `Godot_SendEditorInput`, under a real display, in
  `tools/relay/tests/run_editor_ui_e2e.py`.
- **A replay requirement is not verified by replaying a trace the same process just
  recorded.** Record in one editor run, replay in another.
- **Read the requirement, not the row.** An audit of the interface ledger found five
  rows claiming "none remaining" while a named clause of the requirement did not exist.
- **Do not record a gap as environmental without trying.** `tools/virtual_display.py`
  exists precisely because four requirements were wrongly filed that way.
