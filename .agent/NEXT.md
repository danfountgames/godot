# NEXT

At most five ordered actions. The first must be immediately executable.

1. Add packaging: install the relay next to the editor binary, ship licence notices,
   and add a clean-checkout smoke test. — C2 — verified by building and running both
   from a fresh clone.
3. Give U1/U2 real coverage: drive `MCPApprovalsDialog::refresh()` and the
   approve/revoke paths against a stub settings store. — U1, U2 — verified by those
   tests passing headlessly.
4. Cover F6/F7's untested edges: approval persistence across a restart, and that the
   audit log file is actually written and contains the redacted summary. — F6, F7 —
   verified by tests over a temporary settings/audit location.
