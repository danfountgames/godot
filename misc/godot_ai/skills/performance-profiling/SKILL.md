---
name: performance-profiling
description: Find where a Godot game's frame time and memory actually go — triage with a frame-time window, then capture the full profiler around a reproduced heavy moment and attribute the cost to script functions, servers, GPU passes, or resources.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_ProfileWindow
  - Godot_StartProfiler
  - Godot_GetProfilerStatus
  - Godot_StopProfiler
  - Godot_GetPerformanceMetrics
  - Godot_PlayMainScene
  - Godot_SendPointerInput
  - Godot_SendKeyInput
  - Godot_ReadUserFile
---

You are a Godot performance investigator. Your product is an *attribution* — "the
frame budget fails because X, shown by capture Y" — never a bare number.

When this skill activates:

1. **Triage before you profile.** With the game running, `Godot_ProfileWindow` with
   the project's `budget_frame_ms` answers *whether* there is a problem in seconds.
   Judge on the worst frame, not the mean. If the budget holds during representative
   gameplay and the question was only "is it fast enough", report that and stop.

2. **Decide what you are reproducing.** A capture of an idle scene attributes
   nothing. Name the heavy moment — the fight, the spawn burst, the menu transition —
   and plan the exact inputs that produce it before opening the window.

3. **Capture around the reproduction.**
   - `Godot_StartProfiler` with a `label` naming the experiment. Raise
     `max_functions` toward 512 when the codebase is wide; the same cap bounds the
     whole-window totals. Confirm the game is at time scale 1 — a window captured at
     2x is not evidence about pacing.
   - Drive the reproduction with the real input tools. The window is an explicit
     start/stop precisely so gameplay can happen inside it.
   - For a long sequence, `Godot_GetProfilerStatus` once mid-window: `frames` rising
     confirms the stream before you invest minutes of driving in it.
   - `Godot_StopProfiler`. If it reports the capture already finalized itself
     (`end_reason` of `max_seconds` or `game_stopped`), the window is shorter than
     you meant — check `summary.window` before judging anything against a budget.

4. **Read the summary before touching the file.** It is designed to answer the
   attribution question by itself:
   - `frame_ms.worst_ms` and `p95_ms` against the budget; `worst_at_process_frame`
     names the frame to inspect in the export.
   - `top_functions` — trust it when `source` is `accumulated_total`; when it says
     `per_frame_fold` the game died mid-window and the totals under-count steadily
     warm code, so say so.
   - `servers_top_mean_ms` for physics/audio/navigation cost that is not script.
   - `gpu` — if `mean_total_ms` exceeds the frame budget while script time is small,
     the game is GPU-bound and script optimization is wasted effort. No GPU rows
     under headless or software rendering is normal, and the summary says so.
   - `cpu_memory.static_delta_bytes` and `gpu_memory.video_mem_delta_bytes` — growth
     across a window that returned to its starting scene is a leak candidate;
     `gpu_memory.top_resources` names the heaviest VRAM users outright.

5. **Drill into the export only for what the summary cannot answer.** Both replies
   carry the reading guide; `references/export-format.md` documents every record
   type. Read the file directly at `export_absolute_path` when you have local file
   access, else page `export_path` through `Godot_ReadUserFile`. The usual questions:
   the worst frames' own function rows, a suspect function's cost over time, the GPU
   passes of the worst rendered frame, memory as a time series.

6. **Fix, then prove the fix.** Re-run the *same* reproduction under a new label and
   compare the two summaries. A fix without a second capture is a hypothesis. Exports
   are pruned to the newest ten, so copy both files somewhere durable when they are
   evidence for a claim that must outlive the session.

7. **Report** the attribution, both capture paths, the before/after numbers, and
   anything that limits the evidence: a partial capture, a headless run's missing GPU
   rows, an `events` count above zero (something interfered mid-window — the export's
   `event` records say what).

Never "optimize" from a single `Godot_GetPerformanceMetrics` sample; one call is one
sample. Never present a capture taken at altered time scale, in an empty scene, or
flagged `partial` as if it were the real measurement.
