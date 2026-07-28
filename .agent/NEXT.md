# NEXT

The must-have and high-value tranches are complete and verified, now including the
visual ones: the repository starts its own display (`tools/virtual_display.py`), so
screenshots, dialog clicks and runtime scene-tree inspection are covered by the
end-to-end run rather than deferred to a machine with a screen. At most five ordered
actions, as always.

1. Write the two UI tests that are now possible rather than merely conceivable: open
   the approvals dialog and the command-palette entries under the virtual display and
   click them, the way `click_an_answer()` in `run_editor_e2e.py` already does for
   `Godot_AskUser`. — U1, U2. Nothing environmental is in the way.
2. Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions, and fix
   whatever the runner disagrees with — it now installs `xvfb x11-utils
   libgl1-mesa-dri xdotool` and runs both end-to-end modes. — C1.
3. On a Windows host: run the cross-compiled relay against a Godot editor and run
   `tools/relay/tests/run_tests.py` there. — R8 — the code compiles in CI already.
4. Free-text answers to `Godot_AskUser` remain unverified because no window takes X
   input focus without a window manager (`openbox` did not fix it, and shifts window
   geometry so clicks miss). If this matters, find a focus model that works — otherwise
   leave the limitation recorded in the ledger and `.agent/TACTICS.md`.
5. Only then the optional tranche, in the specification's own order of value:
   O2 (packaged agent backends), O4 (remote HTTP transport), O1 (in-editor chat UI),
   O3 (export-template integration). None is started; none is required.
