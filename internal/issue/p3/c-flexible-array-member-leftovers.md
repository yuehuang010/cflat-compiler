# C flexible / zero-length array member: leftovers after A6 and CFLEX

Summary: A6 (2026-09-28) decays `p->data` to `T*` in every value context. CFLEX (2026-09-29) made `sizeof` of a bare zero-length `[0]` member 0 and refused `sizeof` of a bare true-flexible `[]` member in every spelling (`->`, `(*p).`, `t.`), made `alignof` the element alignment, and promoted anonymous struct/union flexible tails. Probes: scratch/repro_keep/a6r3/ and scratch/repro_keep/cflex/ (main checkout).
Still open:
- `int* p = move a->data;` traps at scope exit (rc 133): `p` adopts an interior pointer. The same happens with any raw local moved into a pointer. Needs a ruling on explicit `move` of a raw pointer (see the explicit-move ruling).
- (P3, master too) `sizeof` of one row of an `int data[0][3]` member gives 4; clang gives 12.
- (P3, master too) `sizeof(*&z->data)` is refused; clang gives 0.
- (P3, master too) A C++ class with both a method and an anonymous struct member cannot reach the anonymous member's fields.
