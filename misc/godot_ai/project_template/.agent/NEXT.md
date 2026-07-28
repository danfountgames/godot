# NEXT

At most five ordered actions. The first must be immediately executable.

1. Read `docs/GAME_SPEC.md` completely, plus any other documents it points to.
2. Health-check the Godot Agent Interface as described in `AGENTS.md`, including
   whether the editor can render (`Godot_GetEditorStatus.can_render`) and whether a
   display and `xdotool` are available for real input. Record it in `.agent/TOOLING.md`.
3. Launch the game as it stands, read the output log, and capture the baseline.
4. Derive the player-facing goal tree into `.agent/GOALS.md`.
5. Choose the smallest complete playable vertical slice and record its hypothesis,
   acceptance route and evidence requirements in `.agent/STATE.md`.
