# NEXT

At most five ordered actions. The first must be immediately executable.

1. Add `Godot_CaptureViewport`: capture the editor viewport to a PNG under the
   project, refusing cleanly when the display server is headless. — T12 — verified by
   a headless test asserting the refusal names the reason; visual correctness needs a
   display and stays unverified until then.
2. Add `Godot_AskUser`: a modal that puts a question to the user and returns the
   answer, with the protocol call held until they respond or a timeout elapses.
   — T13 — verified by tests of the timeout and cancel paths.
3. Port the relay's sockets behind a thin abstraction with a Winsock backend, keeping
   the POSIX path unchanged. — R8 — verified by cross-compiling in CI; runtime
   verification still needs a Windows host.
4. Add packaging: install layout for the relay next to the editor binary, licence
   notices, and a clean-checkout smoke test. — C2 — verified by building and running
   both from a fresh clone.
5. Give U1/U2 real coverage: a test that drives `MCPApprovalsDialog::refresh()` and
   the approve/revoke paths against a stub settings store. — U1, U2 — verified by
   those tests passing headlessly.
