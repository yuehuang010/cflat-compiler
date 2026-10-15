# Join-arm gaps: V13 late-defined callee residual

Found 2026-09-30 by the W4 review; pre-existing on master. Items 1-3 of the original W4 list (C++ ctor
argument `??`/`?:` join typing, direct `new` into a borrowing CFlat callee leaking on unwind, arrow on a
parenthesized `?:` join) were fixed by T236 (legs in test_cpp_interop.cb, T236 block).

## From V13 (2026-10-01, f62b7ad0)
A CFlat callee defined LATER in the file is now treated as retaining for a join-arm `new` argument
(PreserveRetainedJoinArmTempsBeforeCall no longer returns early on a declaration; ParameterRetainsArgument
answers "retains" for an incomplete body). The direct `new` form instead records and resolves the gate
after the walk (ResolveOwnedReleaseGates). Residual: a late-defined callee that does NOT retain and
throws leaks the joined arm on the unwind edge, where the direct form frees it. Fix: record the join
arm like the direct path and resolve after the walk.

2026-10-15 re-probe (T236, /Users/felixhuang/source/cflat-compiler/scratch/repro_keep/t236_rev/late_v13.cb): with a
forward declaration `int LateBorrow(pr.E* q, int t);` and the body after the caller, BOTH the direct
`LateBorrow(new pr.E(5), t)` and the `??` join form leak live=1, including the normal return (t=0).
The "direct form frees it" premise did not reproduce; re-probe before fixing.
- (T236 review 2, pre-existing) a void* arm joined with a new arm (`void* vn = nullptr; pr.Ovv(vn ?? new pr.E(5), 0)`, and the ?: form) is refused; clang gives -5 (void* overload). Direction: an arm with a void pointee joins to void* (MainListener_Expressions.cpp ~7960/~7987). Repro scratch/repro_keep/t236_rev2/void_newco.cb.
