# Taking a field address of a by-value parameter for a read is refused as a consume

Repro: scratch/repro_keep/box1022/t412_ro_alias.cb (found by T412 review, 2026-10-22).
`void f(Box b) { list<int>* p = nullptr; p = &b.xs; int n = p->count(); }` is refused:
"cannot consume field 'Box.xs' out of borrowed parameter 'b' - ... (double-free)". Nothing is consumed:
`p` is a borrowed pointer used for a read-only call. The `p = &b.xs` assignment into a pointer local is
classified as a field move out of the borrowed parameter.

Fix direction: an address-of store into a raw (non-owning) pointer local is a borrow, not a consume;
only a store into an owning slot / move sink should hit this refusal. Keep the T412 double-free refusal for
real consumes (owning field moved out of a borrowed param).
