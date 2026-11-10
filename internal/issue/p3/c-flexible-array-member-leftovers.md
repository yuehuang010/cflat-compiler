# C flexible / zero-length array member: leftovers after A6 and CFLEX

**RULED 2026-10-24 (main session; maintainer may veto):** `move a->data` (flexible / zero-length array member) is REFUSED: the member is an array, not a pointer slot, so there is nothing to transfer and nothing to null; `move` of any fixed or flexible ARRAY member is an error ("cannot move an array member; take its address or copy the elements"). Same for a raw local pointing into another object (interior pointer) - covered by the general explicit-move-nulls-source rule only for owning slots.

Summary: A6 (2026-09-28) decays `p->data` to `T*` in every value context. CFLEX (2026-09-29) made `sizeof` of a bare zero-length `[0]` member 0 and refused `sizeof` of a bare true-flexible `[]` member in every spelling (`->`, `(*p).`, `t.`), made `alignof` the element alignment, and promoted anonymous struct/union flexible tails. Probes: scratch/repro_keep/a6r3/ and scratch/repro_keep/cflex/ (main checkout).
Still open:
- Explicit `move` of a raw local or parameter pointer that points into another object (`int* data = &x; int* p = move data;`) traps at scope exit (rc 134, master too); needs a ruling under the explicit-move rule. (Array members are refused since T489.)
- (P3, master too) A C++ class with both a method and an anonymous struct member: NOT reproduced (class, const method, ctor, static factory, pointer, member-before-method shapes all match clang++). Needs a repro from the reporter.
- (sibling, plain arrays, not CFLEX) `auto row = m[0];` for `int[2][3] m` decays to `int*` (sizeof 8; clang 12). This semantic question remains out of scope.

FIXED in UQRAW for the address-of shapes (narrowed; see the note at the end of this paragraph). RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31): explicit `move` of a raw pointer whose borrowed provenance is
PROVEN (address-of a local or field, a borrowed pointer parameter, a local copied directly from one) into any slot is REFUSED at compile
time: "cannot move a borrowed pointer; move from the owning slot". Same polarity as the 2026-09-03 slot-move rule (unknown provenance
accepts) and the 2026-10-25 line that explicit move nulls only OWNING slots; the scope-exit trap (rc 134) becomes a located error
(refuse first, refine later). Covers the sibling shape `unique<int>* p = &u; unique<int>* q = move p;` (box 7 MIX12 finding, probe
scratch/repro_keep/mix12/mix12_tmp/own11.cb, rc 134 on master). Test consequence: one new err_ leg each for the int* and unique<T>*

RULED 2026-11-04 (maintainer): the 2026-10-31 refusal is narrowed to ADDRESS-OF provenance (what UQRAW landed). `move` of a
borrowed pointer parameter, or a local copied from one, into a plain `T*` slot is a plain copy - no adoption, no nulling,
same line as the 2026-10-25 `move p->f` through-a-borrow ruling (Rust copies a moved `&T`, C++ std::move of a raw pointer
copies). `move` into a C++ `T*&&` parameter is an xvalue cast for the overload pick, not a transfer. Test consequence: none -
test_move bpm* legs and test_cpp_interop T248 (== 307) stay; this OPEN item closes.

CLOSED 2026-11-04 by the ruling above: borrowed-param move stays a copy (bpm legs, T248 stay); no code change.

Fixed in UQRAW (address-of provenance only): explicit `move` of an ADDRESS-OF binding (`int* p = &x;`, `&s.v`, `unique<int>* p = &u;`) into a
raw local, a unique local, or by assignment is refused with the ruled text. Legs: Test/errors/err_move_borrowed_addr_slot.cb,
err_move_borrowed_unique_slot.cb, err_move_borrowed_assign_slot.cb. NOT fixed, needs a maintainer ruling: (1) a borrowed (non-move)
POINTER PARAMETER, or a local copied from one, moved into a PLAIN `T*` slot stays accepted - the existing test_move.cb bpm* legs
(`Resource* d = move p;`, "must NOT adopt") pin it, and the ruling's "any slot" wording contradicts them; (2) address-of into a C++
`move` pointer parameter stays accepted - test_cpp_interop.cb T248 leg (`CallT248DataPick(move t248DataMove)` == 307) pins it.
shapes; no accept leg flips.
