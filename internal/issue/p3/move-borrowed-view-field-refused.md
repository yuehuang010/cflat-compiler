# `move` of a borrowed view field (`int[] v`) is refused outside a local declaration

Residue of the movecopy fix (2026-11-01). The 2026-10-25 ruling makes `move p->f` of a non-owning value field a
plain copy. An element-pointer view field (`int[] v`) can OWN its buffer (`v = new T[n]`), so a `move` of it
through a borrowed pointer is a transfer and stays refused when it feeds a move sink or an owning slot.

Done: `T[] r = move p->v;` (a local declaration initializer) copies the view, source intact, leg in
Test/test_move.cb (`mc_move_view_*`).

Still refused (safe, master message): `move p->v` as a move-sink argument, into a global or a field, as an
assignment RHS, as a `return`, and as a non-move parameter argument. The last one is the open question: a non-move
parameter is a non-owning destination and could copy like a local, but needs callee resolution at the argument.

Acceptance: decide the non-move parameter and assignment-to-local shapes; each needs a leg in
Test/test_move.cb or Test/errors/. Delete this file when they are covered.
