# Returning a heap array through release is refused

Summary: Returning `h->u.release()` as a wrapper pointer is rejected despite complete types.
Repro: `return h->u.release();` into a `Wrap<S2>*` return.
Master vs expected: master reports `cannot return the heap array from new...`; expected the released pointer to satisfy the return type.
Found: T432 r2 opus review, 2026-10-24 box, PRE-EXISTING
