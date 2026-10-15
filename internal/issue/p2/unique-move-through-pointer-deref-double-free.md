# `move *p` through a `unique<T>*` aborts (likely double free)

Found by T196 Opus r1 (2026-10-14), pre-existing on master. Repro scratch/repro_keep/t196_rv1/rev_t196_opus_unique_ptr_move.cb:
`unique<Res> owner = unique<Res>(new Res()); unique<Res>* p = &owner; unique<Res> taken = move *p; return 0;` exits 134.
Control `unique<Res> taken = move owner;` exits 0. Likely `move *p` transfers the pointee but does not null the source slot
through the deref, so owner and taken both free it (unconfirmed - check the IR). Fix: explicit move nulls its source
(explicit-move-nulls-source ruling) for a deref lvalue too, or refuse `move *p` with a message if the slot cannot be nulled.
