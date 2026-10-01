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
