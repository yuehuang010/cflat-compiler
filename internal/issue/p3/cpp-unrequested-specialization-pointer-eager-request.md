Bucket: p3 (C++ interop compile time; left by D6, fix/unrequested-spec-pointer-retry, 2026-09-29)

# Pointer-to-unrequested-specialization member results are requested at member projection, not at use

D6 made a member whose return type is a pointer to a not-yet-requested class template specialization
(`Box<int>* nul()`) request that specialization so `o.ptr()->v`, `auto q = o.ptr(); q->v` bind. The request
fires when the member is projected (`TypeHasMember` -> `EnsureCxxMemberProjected` on the `.`/`->` token,
MainListener_PostfixExpression.cpp ~1296), so a call that never dereferences the pointer still pays it:
`auto p = o.nul(); p != nullptr` costs ~2 incremental clang stages per distinct pointee (master 0).
Probe: scratch/repro_keep/d6/rv1/b_store.cb (`bash pr.sh b_store.cb bs` there counts stages).
Params never request (only the selected member's return type does).

Fix direction: record the pointee spelling on the projected member and request it at the first member
access / deref / `->` through a value of that opaque pointer type. Gating the projection site alone is
wrong: the member is then marked projected and a later use never requests (the stored-pointer legs fall
back to the refusal). A round-2 attempt gated a LATER call site instead, which had no effect (projection
had already requested), and was removed.
