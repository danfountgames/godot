# NEXT

At most five ordered actions. The first must be immediately executable.

1. Put the relay's socket calls behind a thin platform seam (`socket_open`,
   `socket_connect`, `socket_recv`, `socket_send`, `socket_poll`, `socket_close`)
   with a POSIX backend that behaves exactly as today. — R8 — verified by the relay
   suite still passing 39/39 with no behaviour change.
2. Add the Winsock backend behind that seam, plus stdin handling that works on
   Windows (a pipe is not pollable with WSAPoll). — R8 — verified by cross-compiling
   in CI; runtime verification still needs a Windows host and stays BLOCKED.
3. Add packaging: install the relay next to the editor binary, ship licence notices,
   and add a clean-checkout smoke test. — C2 — verified by building and running both
   from a fresh clone.
4. Give U1/U2 real coverage: drive `MCPApprovalsDialog::refresh()` and the
   approve/revoke paths against a stub settings store. — U1, U2 — verified by those
   tests passing headlessly.
5. Cover F6/F7's untested edges: approval persistence across a restart, and that the
   audit log file is actually written and contains the redacted summary. — F6, F7 —
   verified by tests over a temporary settings/audit location.
