# `look(c ? new T(x) : p)` leaks the `new` arm

Bucket: p3 (bounded leak, never an early free). Pre-existing on master.

## Repro

`int look(T* p);` then `look(c > 0 ? new T(5) : n);` with c=1: one new, zero dtors. Same for
`look((c > 0 ? new T(8) : n) ?? new T(9))` with c=1 (the `??` RHS path is fine: c=0 frees 9).

## Root cause

PropagateTernaryOwnership treats an owning/borrow pointer join as owning nothing
(SuppressCallerRelease on the PHI), and FinishTernaryArm's call-argument hoist runs with
includeBareNew=false, so the bare-`new` arm is never ledgered.

## Fix direction

In a call argument, hoist a bare-`new` arm into the conditional slot (includeBareNew) like the
`??` RHS; DropRetainedJoinArmPtrTemps already keeps it alive when the callee retains the join.
