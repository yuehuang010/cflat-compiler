# Ternary arm into a `move` parameter: field arm and duplicated borrowed arm still double-free

PRE-EXISTING (found by T107 Haiku). T112 fixed the owning-LOCAL arm (false/true arm, both arms,
nested, parenthesized, method move param): the arm is an implicit consume like `mv(n)`,
nulled on the taken path, later read is "use of moved variable". Two cells stay open.

## 1. Owning field arm
```cflat
int r = mv(c > 0 ? new P() : s.p);   // c == 0: s.p consumed by mv, freed again with s -> ASAN double-free
```
The direct form `mv(s.p)` is correct. Routing field conditional sources into the move-param transfer
(`|| params[i].IsMove` in ApplyMoveParamTransfer, LLVMBackend_WinRT.cpp ~1903) breaks the existing
unique-field ternary ASSIGN legs (test_move.cb ~1348: `c.item = cond ? a.item : b.item` must leave
the selected field readable as null), so it needs a discriminator between a move-param sink and that
assignment join.

## 2. Same borrowed pointer in both arms
`mv(c ? b : b)` with a borrowed `b` compiles and double-frees; the direct `mv(b)` is refused. Matching
the direct form adds a rejection (borrowed arm into a move sink) - needs a ruling on the surface; see
the move-borrow-into-sink slot rule (reject move of a borrowed pointer into a move sink).

## 3. `??` operands into a move sink
`mv(c ? new P() : (a ?? n))` with null `a`, `a ?? n` on its own, and an owning `??` LHS still
double-free (pre-existing). T112 r2 tried registering an owning-local `??` RHS as a conditional
source at the `??` join (ParseConditionalExpression, rhsBr block); Sol review 1 found it also
marked `n` moved at a NON-move unique-field assignment `s.p = a ?? n;` (repro
scratch/repro_keep/t112/rev_t112_coalesce_field.cb, master exits 30), so it was removed. A fix must
scope the consume to a move sink and keep nested RHS ledgers (`a ?? (c2 ? n : m)`,
rev_t112_coalesce_nested.cb).
Probes: scratch/repro_keep/t112/ (matrix scripts from T112 round 2).
