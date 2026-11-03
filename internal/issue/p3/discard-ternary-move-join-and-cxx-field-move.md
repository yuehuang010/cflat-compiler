# Discard sink leftovers after T352: `?:` move join leaks, C++ class field move is a no-op

Severity p3. Found by the T352 review (2026-10-20); pre-existing on master (same result before and after T352).
Cases: scratch/repro_keep/box1021/t352_rev/cases/ (ternary_move.cb, cxx_field.cb + cx.hpp + cxx_oracle.cpp, owning_raw_field.cb).

1. Fixed in MIX19: `_ = c ? move o.s : move q.s;` (and `string r = c ? move o.s : move q.s;`, call arguments, parenthesized
   arms) no longer leaks. Root cause (MIX19): the join deep-copied each moved string arm (AdoptTernaryStringArm) after the move
   had zeroed its source, so the original buffer had no owner. Fix: FinishTernaryArm(movedString) keeps the moved buffer when
   both arms are explicit moves; the join's existing RegisterOwnedStringTemp(phi) frees the selected one. Legs:
   Test/test_move.cb testMix19TernaryMoveJoin. Not the discard sink: the sink already returns early on a join that the join owns.
   Still open (same root cause, left as is per the MIX19 brief): a mixed join `_ = c ? move o.s : q.s;` with the move arm
   selected still leaks (scratch/mix19_tmp/p1.cb case 3, c=1). Root cause (MIX19): same deep copy, one arm not a move.
2. Not fixed in MIX19 (optional item, gated on item 3 being fixed). `_ = move p->c;` on a C++ class field runs no move ctor
   and leaves the field intact (len 66); clang twin `{ Cx t = std::move(p->c); }` gives move=1 dtor=2, len 0.
   Root cause (MIX19): the discard branch handles foreign C++ ELEMENTS (IsElementAccess, MainListener_Expressions.cpp
   ~4760) but a C++ class field falls through to RegisterDiscardedOwningStructTemp, which returns early on a storage-backed
   value (MainListener_PostfixExpression.cpp ~18253), so no move-ctor temp is built.
3. Not fixed in MIX19 (by design, not a fix candidate). `_ = move o.raw;` where the user dtor deletes the raw field leaks.
   Root cause (MIX19): a raw `Val*` owns nothing (raw lifetimes are untracked, memory-safety-function-boundaries-only ruling).
   The move nulls the field, so ~O deletes nothing, and discarding a non-owning pointer has no drop. The owning spelling
   `Val* p = move o.raw; delete p;` is leak-free (owning_raw_field.cb recorded in scratch/mix19_tmp/p2.cb case 3).
   Clang twin: a raw pointer move is a copy with no delete, so clang also frees nothing here.

Open after MIX19 round 2 (measured on master and on the fix; each leaks the same on both unless noted):
4. Nested all-move join `_ = c ? move a : (d ? move b : move x);` leaks 1 at c=1 (master 1). The outer arm is a
   parenthesized `?:`, which the move-arm shape test does not see as a move. Root cause (MIX19): armIsMove
   (MainListener_Expressions.cpp ~13186) stops at a non-move nested join.
5. Assignment `r = c ? move a : move b;` leaks 1 (master 2; the fix removed one). Root cause (MIX19): the string
   assignment copies the join's selected buffer (deep copy at MainListener_Expressions.cpp ~9852, the join result is
   not IsMove), while the join's RegisterOwnedStringTemp(phi) still owns the buffer. A fix needs both: skip the copy
   and unregister the phi on store, across the string-assign arms. Optional item of round 2, not attempted: not small.
6. Struct-with-dtor field arms `_ = c ? move o.d : move q.d;` (`struct DT { int* p; ~DT() { delete p; } }`) leak the
   dtor's int* (master 1, same). Root cause: not investigated in MIX19 (the round-1 join fix covers strings only).
7. Converting-constructor gap (pre-existing on master): `A a = mkB();` is refused when A has a converting ctor A(B),
   although the converting-constructor ruling (string-literal-cpp-class-ruling.md) says it should convert. Master
   reports the decay text; MIX19 reports "cannot initialize or assign 'A' from unrelated struct type 'B'". Not MIX19's fix.
