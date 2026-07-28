# NEXT

At most five ordered actions. The first must be immediately executable.

1. Give `MCPApprovalsDialog` headless coverage: build it with a stub service, call
   `refresh()`, and assert it lists pending clients and every discovered skill with
   the right status and buttons. — U2 — verified by that test passing headlessly.
2. Cover the palette/menu registration: assert the commands exist after the service
   enters the tree. — U1 — verified by the same test run.
3. Cover approval persistence: an `EditorSettings` stand-in so approve/revoke can be
   asserted to survive, closing the last F6 caveat. — F6 — verified headlessly.
4. Re-read `docs/godot-ai-clone-spec.md` end to end against the ledger and correct any
   entry whose evidence no longer matches the code. — all — verified by the audit
   finding no discrepancies.
5. Consider the optional tranche (O1–O4) only after the above: in-editor chat UI,
   packaged agent backends, export-template integration, remote HTTP transport. Each
   is explicitly optional in the specification and none is started.
