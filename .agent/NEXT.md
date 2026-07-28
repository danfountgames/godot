# NEXT

At most five ordered actions. The first must be immediately executable.

1. Give the protocol a deferred-response path: `MCPProtocol::handle_message` gains a
   way for a tool to say "answer later", and `MCPService` holds the request id until
   the tool completes. Nothing may block the editor's main thread. — prerequisite for
   T13 — verified by a test that a deferred call produces exactly one response, later.
2. Add `Godot_AskUser` on top of that: a modal with the question and the choices,
   returning the user's answer, with cancel and timeout paths. — T13 — verified by
   tests of both non-answer paths.
3. Port the relay's sockets behind a thin abstraction with a Winsock backend, keeping
   the POSIX path unchanged. — R8 — verified by cross-compiling in CI; runtime
   verification still needs a Windows host.
4. Add packaging: install layout for the relay next to the editor binary, licence
   notices, and a clean-checkout smoke test. — C2 — verified by building and running
   both from a fresh clone.
5. Give U1/U2 real coverage: drive `MCPApprovalsDialog::refresh()` and the
   approve/revoke paths against a stub settings store. — U1, U2 — verified by those
   tests passing headlessly.
