# Discard sink leftovers after T352: `?:` move join leaks, C++ class field move is a no-op

Severity p3. Found by the T352 review (2026-10-20); pre-existing on master (same result before and after T352).
Cases: scratch/repro_keep/box1021/t352_rev/cases/ (ternary_move.cb, cxx_field.cb + cx.hpp + cxx_oracle.cpp, owning_raw_field.cb).

1. `_ = c ? move o.s : move q.s;` (string fields) leaks the selected buffer (HeapAudit L=1): the ?: join result is not
   IsExplicitMove, so RegisterDiscardedOwningStructTemp (MainListener_PostfixExpression.cpp ~12311) skips it.
2. `_ = move p->c;` on a C++ class field (`struct CH { Cx c = default; }`) runs no move ctor and leaves the field intact (len 66);
   clang twin `{ Cx t = std::move(p->c); }` gives move=1 dtor=2, len 0.
3. `_ = move o.raw;` where the user dtor deletes the raw field leaks.
Fix direction: item 1 - treat a ?: / ?? join whose arms are all explicit moves as an explicit move in the discard sink; item 2 - the
discard of a moved C++ class lvalue materializes a temporary via the move ctor and destroys it (clang semantics); item 3 needs a look.
