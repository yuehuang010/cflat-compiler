# C flexible / zero-length array member: leftovers after A6 and CFLEX

**RULED 2026-10-24 (main session; maintainer may veto):** `move a->data` (flexible / zero-length array member) is REFUSED: the member is an array, not a pointer slot, so there is nothing to transfer and nothing to null; `move` of any fixed or flexible ARRAY member is an error ("cannot move an array member; take its address or copy the elements"). Same for a raw local pointing into another object (interior pointer) - covered by the general explicit-move-nulls-source rule only for owning slots.

Summary: A6 (2026-09-28) decays `p->data` to `T*` in every value context. CFLEX (2026-09-29) made `sizeof` of a bare zero-length `[0]` member 0 and refused `sizeof` of a bare true-flexible `[]` member in every spelling (`->`, `(*p).`, `t.`), made `alignof` the element alignment, and promoted anonymous struct/union flexible tails. Probes: scratch/repro_keep/a6r3/ and scratch/repro_keep/cflex/ (main checkout).
Still open:
- Explicit `move` of a raw local or parameter pointer that points into another object (`int* data = &x; int* p = move data;`) traps at scope exit (rc 134, master too); needs a ruling under the explicit-move rule. (Array members are refused since T489.)
- (P3, master too) Implicit-this `move data` of an array member inside a method is refused with an unrelated message (aggregate cast / return mismatch) instead of the T489 text; `move this->data` is correct.
- (P3, master too) `sizeof` of one row of an `int data[0][3]` member gives 4; clang gives 12.
- (P3, master too) `sizeof(*&z->data)` is refused; clang gives 0.
- (P3, master too) A C++ class with both a method and an anonymous struct member cannot reach the anonymous member's fields.
