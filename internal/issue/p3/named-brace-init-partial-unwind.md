# Named brace init: `{ h = move *p }` refused

Out of scope of T261 (partial-unwind fix landed). A named field initialized with a move of a
dereferenced raw pointer is refused "dereference of moved variable 'p'" on master and after the fix;
clang moves from `*p` (probe case c01 in the T258 review repro set).

## Constructor-then-override path (T261 review 1, same on master; repros scratch/repro_keep/t261/rev_t261_repro/cases)
A named brace init on a struct without a foreign C++ field, or with a user no-arg ctor, runs the ctor and then
EmitFieldInitializer with destinationIsFresh=false (MainListener_Declarations.cpp ~10196); nothing is registered
for unwind, so a later throwing field leaves the built object undestroyed (p25 unique<Own> field leaks, p29 4 built
/ 2 destroyed, p31, p01/p26). Same shape at MainListener_Statements.cpp ~5468 (default param `T u = {...}`) and
MainListener_PostfixExpression.cpp ~7909 (call argument `f({...})`).
A round-2 attempt (scratch/repro_keep/t261/t261_round2.patch, review t261_review2.md) registered the whole object as
a Slot and suspended ALL Slot entries across drop-old-then-store: fixed p01/p25/p26/p29/p31 but leaked the outer
objects of a nested override whose copy throws (rev_t261_q13_outer_suspend.cb: master 5 dtors, patch 3), and the
window still leaks the object's other built fields (q12, tag form q17; master too). An outer object's full dtor
would destroy the dropped field again, so suspending only the own entry is not enough.
Fix direction: do not seed fields that the brace init names (construct only the unnamed ones, then construct each
named field in place under per-field registration), so there is no drop-old window at all.
Also pre-existing: a struct passed by value from a brace (`use({...})`, `use(u: {...})`, default param) is never
destroyed on the normal path.
