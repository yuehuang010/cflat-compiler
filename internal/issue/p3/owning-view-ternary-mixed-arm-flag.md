Bucket: p3 (leak; safer than before - the other arm used to double free)

# A `?:` mixing a `new` arm and a borrowed arm leaks the `new` block of an owning view

Found 2026-09-23 in review of fix/owning-view-reassignment-release (macOS arm64, Release).

## Repro

```cflat
S[] w = new S[3];
S[] v = new S[1];
v = zero == 0 ? new S[2] : w;   // new arm taken: S[1] released, S[2] never freed (HeapAudit +1)
```

scratch/rv/r_ternary_mixed2.cb: master run=0 leak=1 (count lost); fix branch run=0 mid d=1,
d=4, leak=1 (the S[2] block). With the BORROW arm taken (scratch/rv/r_ternary_mixed.cb) master
double freed (run=133); the fix branch runs clean (d=4 leak=0).

## Root cause

The view's runtime owns-its-block flag (`<name>.owns`, NamedVariable::ViewOwnFlag) is set by
the assignment's adopt step as ONE value for the whole `?:` result. A join of an owned arm and a
borrowed arm does not adopt, so the flag stays false on the path where the `new` arm was taken.

## Fix direction

Set the flag per arm: `flag = select(cond, armOwns0, armOwns1)` from the join's arm provenance
(the owned-arm ledger the '?:' lowering already keeps), instead of a single adopt decision.
Acceptance: both arm directions with exact destructor counts and HeapAudit unchanged.
