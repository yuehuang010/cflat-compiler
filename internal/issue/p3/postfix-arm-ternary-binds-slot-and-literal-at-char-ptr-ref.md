# Ternary arm lvalue leftovers after T325: scalar postfix arm binds the slot; string literal arms bind char*&

Found by T325 review 1 (2026-10-16), both PRE-EXISTING on master.
- `ibump(t ? i++ : j)` (C++ `int&` param) binds `i` itself (i ends at 2); clang refuses (`i++` is a prvalue). The arm-storage
  load fallback in MainListener_Expressions.cpp (ternary arms, "An arm value loaded from storage keeps that storage") takes
  the slot of a load that is followed by a store to the same slot. T325 guards only its pointer join (loadSlotRewrittenAfter);
  the scalar join (scalarLvalueJoin) needs the same guard, minus explicit `move` arms (they also store the source).
- `cbump(t ? "ab" : "cd")` with a C++ `const char*&` param is accepted; clang refuses.
  Note: a false-arm postfix store is queued in PlusPlus until the arm's postfix flush (not yet emitted at the scan) - T325 checks PlusPlus.count(slot) too; copy both checks.
