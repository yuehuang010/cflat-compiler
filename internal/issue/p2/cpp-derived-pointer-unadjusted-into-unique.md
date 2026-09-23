# `unique Base* = new Derived()` does not base-adjust a non-primary base

Found 2026-09-23 while fixing the reference-return-at-return issue (macOS arm64, Release). Pre-existing,
silent wrong value.

## Summary

With `struct B : L, R` (R the NON-primary base), `unique R* r = new B();` and
`unique R* make() { return new B(); }` both hold B's address unadjusted: `r.rv` reads L's field
and `r.r()` dispatches through the wrong vtable. `move R* make() { return new B(); }` is correct
(the return-site adjust applies to a pointer return type).

## Repro

scratch corpus `rr_q19.cb` (declaration) and `rr_q16.cb` (return): both exit 1, expected 0.

## Fix direction

Apply `AdjustCxxPointerForStore` against the unique<T> pointee before the raw pointer is adopted
(`CreateCoreUniqueFromRawPointerCall` callers: declaration desugar and `EmitReturnExpression`'s
`rawPointerReturn` block). Deleting through the adjusted pointer must stay correct - it goes
through the virtual deleting destructor when the base has one; a base without a virtual
destructor needs a diagnostic, as in C++ it is undefined behaviour.
