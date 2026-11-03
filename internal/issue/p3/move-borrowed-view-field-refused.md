# `move` of a borrowed view field (`int[] v`) is refused

Residue of the movecopy fix (2026-11-01). The 2026-10-25 ruling makes `move p->f` of any non-owning value field a
plain copy; an element-pointer view field (`int[] v`) through a borrowed pointer is still refused (safe, not a copy).
Acceptance: `move p->v` copies the view (source intact) like a scalar field; leg in Test/test_move.cb.
