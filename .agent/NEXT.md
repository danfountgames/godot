# NEXT

At most five ordered actions. The first must be immediately executable.

1. Add the approvals UI: an editor settings section (or dock) listing pending clients
   and discovered skills with allow/deny buttons, wired to
   `MCPService::approve_client_name`/`revoke_client_name` and `MCPSkills::set_allowed`.
   — U2 — verified by tests of the underlying calls plus a manual check.
2. Register command palette entries for the service (status, restart, approve pending
   clients). — U1 — verified by the entries appearing and invoking the right calls.
3. Add a headless execution hook: a CLI entry point that runs one tool call without
   the editor UI, for scripted automation. — U3 — verified by a test invoking it and
   asserting the JSON result on stdout.
4. Add `Godot_CaptureViewport`, refusing cleanly in headless runs. — T12 — verified by
   a headless test asserting the refusal; visual verification needs a display.
5. Port the relay's sockets behind a thin abstraction with a Winsock backend so R8
   stops being blocked on Linux-only code. — R8 — verified by cross-compiling in CI;
   runtime verification still needs a Windows host.
