# Join arm `new` passed to a retaining CFlat callee that throws is freed on unwind (remaining cases)

Found 2026-09-30 by the W4 review. Directly defined-earlier CFlat callees (plain, wrapper, recursive,
generic, struct method) fixed by W10 (2026-09-30). Rule: a joined arm behaves exactly like the same
expression written as a direct `new` argument, on normal and exceptional edges.

## Remaining (both also on master, rc=133 double free; direct `new` form is correct)
1. Callee defined LATER in the file: LLVMBackend_OwnershipTemps.cpp ~2192 returns early when
   `callee->isDeclaration()`. Repro scratch/repro_keep/w10/rev4/a.cb `cfLate` (forms coR/coL x=1,3,
   nest/terc x=3). The direct path resolves this after the walk (ResolveOwnedReleaseGates,
   LLVMBackend.cpp ~2408); the joined path needs the same record-then-resolve instead of the early
   return (or fall to "retains").
2. Call through a function value: `Lambda<int(pr.E*, int)> f = cfKeep; f(empty ?? new pr.E(5), 1)`
   (scratch/repro_keep/w10/rev4/b.cb F_co). Indirect calls never reach the join-arm gate
   (MainListener_ControlFlowAndFunctions.cpp ~707, 826); unknown callee should be treated as retaining.

## Note
Callee that throws BEFORE storing (`cfKeepAfter`) now leaks the arm, same as the direct form (was
freed on master) - consistent with the oracle; the direct-form leak is p3 join-arm-small-gaps-after-w4.
