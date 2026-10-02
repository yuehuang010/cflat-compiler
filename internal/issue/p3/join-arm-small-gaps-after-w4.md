# Join-arm small gaps left after W4

Found 2026-09-30 by the W4 review; all pre-existing on master. Probes:
scratch/repro_keep/w4/rev/ (probe.h counting header).

1. C++ constructor argument: `pr.B(empty ?? new pr.E(5), 0)` -> "has no constructor whose
   parameter types match these arguments ('', 'int')" - the join's type is lost before
   constructor overload resolution.
2. Direct `new` into a CFlat BORROWING callee that throws leaks: `cfBorrow(new pr.E(5), 1)`
   leaves live=1; the `??` join form frees it (join is right, direct path is the outlier).
3. Arrow on a parenthesized `?:` join: `(c ? a : n)->v` -> "Undefined variable v." W4 fixed
   only the `??` form (MainListener_PostfixExpression.cpp ~1255).

## From V13 (2026-10-01, f62b7ad0)
A CFlat callee defined LATER in the file is now treated as retaining for a join-arm `new` argument
(PreserveRetainedJoinArmTempsBeforeCall no longer returns early on a declaration; ParameterRetainsArgument
answers "retains" for an incomplete body). The direct `new` form instead records and resolves the gate
after the walk (ResolveOwnedReleaseGates). Residual: a late-defined callee that does NOT retain and
throws leaks the joined arm on the unwind edge, where the direct form frees it. Fix: record the join
arm like the direct path and resolve after the walk.
