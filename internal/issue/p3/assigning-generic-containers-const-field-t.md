# hpc/btree.cb rejects a KEY type K with a const field

Summary: T451 (2026-10-23) fixed the value side - com.cb `HResult<T>.succeed` constructs in place, and btree's V-assigning
methods are guarded by `if const (is_assignable(V))` + compile_error, so `btree<K, ConstFieldV>` instantiates and lookups work.
Still open: `btree<CK, int>` where CK has a const field (with operator< and copy()) fails to compile even for `contains`:
"cannot assign a whole 'CK': field 'id' is const" at cflat/core/hpc/btree.cb `_storeKey` (`n->keys[i] = key.copy()`).
Key-slot writers without an is_assignable(K) guard: `_storeKey`, `_clearKey`, `_insertChildAt`, `_splitChild`, `_leafOpenSlot`,
remove, merge/borrow.
Repro: scratch/repro_keep/box1022/t451/rev_t451_opus_constkey.cb.

Fix direction: guard the key-slot writers with `if const (is_assignable(K))` + compile_error like the V side (lookups must
still compile), or placement-construct keys into destroyed/uninitialized slots (node layout change).
