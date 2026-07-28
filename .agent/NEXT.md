# NEXT

Every requirement in the specification is implemented, and all but two are verified
here. What remains needs a machine this is not.

1. Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions, and fix
   whatever the runner disagrees with. — C1. The workflow installs
   `xvfb x11-utils libgl1-mesa-dri xdotool` and runs both end-to-end modes plus the
   UI run; every command passes locally.
2. On a Windows host: run the cross-compiled relay against a Godot editor and run
   `tools/relay/tests/run_tests.py` there. Do the same on macOS. — R8. Both backends
   compile in CI, and `platform::initialize()` is now actually called, which it was
   not before the HTTP work went in — so the Windows path has never run *correctly*.
3. Catch `notifications/cancelled` end to end: cancel a chat turn in flight from the
   dock and assert the client is told to stop. The conversation's half is unit-tested;
   the frame is not. Needs the Cancel button located on screen the way
   `click_first_action()` finds the approvals dialog's buttons.
4. Nothing else is outstanding. New work should start from the specification or from
   a user request, not from this file.
