# Unique foreach element adoption leftovers (residue of p2 unique-foreach-element-and-param-copy-into-field-double-free)

Bucket: p3. Filed 2026-11-02 at the fe2field landing. The landed fix moves a `for (unique P* p in c)` element
(list<T>, array<T>, fixed array) out of its slot when a unique field adopts it (`t.p = p`, `?:` / `??` around p):
after the adoption the live slot (recomputed from the container's buffer/count, guarded by adopted == the
iteration's element) is nulled if it still holds the adopted pointer. Each shape below behaves the same on master
(abort 134 or 139 under MallocScribble=1 / Guard Malloc). Repros: fe2field worktree reviews, copied to
scratch/repro_keep/fe2field/ (main checkout, untracked).

1. Adoption inside a capturing lambda in the loop body - the slot is not traced through the capture.
2. `a.removeAt(0)` (any shift) before the adoption: the adopted element now sits in another slot, the
   recorded index misses it -> double release.
3. Reading the loop variable `p` after the body grew the list (`p` is bound to the old slot address) reads freed
   storage (n_alias_grow, n_nested_inner).
4. Implicit `sink(yes ? p : nullptr)` into a `move P*` parameter double releases (no slot clear on the sink path).
5. Collections other than list<T> / array<T> / fixed arrays (iterator-protocol and other `_ptr` containers) get no
   slot clear (master behaviour).
Fix direction: for 2-3 the borrow rule says a container must not be mutated while a binding into it is live -
refuse growth/removal of the iterated container inside the body (Rust's borrow of the iterator), or re-find the
element by value; 1 and 4 route through the same adoption hook.
