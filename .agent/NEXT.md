# NEXT

The must-have and high-value tranches are complete and verified. What remains needs
either a display, another operating system, or a decision to start the optional
tranche. At most five ordered actions, as always.

1. On a machine with a display: verify `Godot_CaptureViewport` produces a correct
   image and `Godot_AskUser` returns a clicked answer, and exercise the approvals
   dialog by hand. — T12, T13, U1, U2 — these are the only gaps that are purely
   environmental.
2. On a Windows host: run the cross-compiled relay against a Godot editor and run
   `tools/relay/tests/run_tests.py` there. — R8 — the code compiles in CI already.
3. Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions, and fix
   whatever the runner disagrees with. — C1.
4. Runtime inspection of a *windowed* game: `Godot_GetRuntimeSceneTree` and
   `Godot_SetRuntimeProperty` are implemented and refuse correctly, but a headless
   game never reports its tree, so the success path is unproven. — T15 follow-up.
5. Only then the optional tranche, in the specification's own order of value:
   O2 (packaged agent backends), O4 (remote HTTP transport), O1 (in-editor chat UI),
   O3 (export-template integration). None is started; none is required.
