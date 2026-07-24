# Reassigning a `unique` pointer local via `move` (not decl-init) loses ownership tracking

Opened while testing deref-of-moved-pointer-across-blocks-not-diagnosed.md. Tripped over
directly while writing regression coverage for that unrelated fix; not caused or fixed by
it, and not investigated further than the repro below.

## Summary

```cflat
struct R { int v = 0; ~R() { printf("dtor\n"); } };
extern int main() {
    unique R* k = new R();
    k->v = 17;
    unique R* k2 = nullptr;
    k2 = move k;             // plain '=' reassignment, not a decl-initializer
    return 0;
}
```

prints nothing - `k2`'s destructor never runs at scope exit, even though `k2` now holds a
live object moved in from `k`. The object leaks.

The decl-init form does not have this problem:

```cflat
struct R { int v = 0; ~R() { printf("dtor\n"); } };
extern int main() {
    unique R* k = new R();
    k->v = 17;
    unique R* k2 = move k;   // decl-initializer, not a reassignment
    return 0;
}
```

correctly prints `dtor` once at scope exit.

## Root cause

Not investigated in depth (out of scope for the fix that surfaced it). The decl-init path
(`unique R* k2 = move k;`, inside `ParseDeclaration`) has an explicit `srcIsOwningMove`
branch that sets `IsOwning`/`IsNewAllocated` on the new local and nulls the source (see
MainListener.h, the `unique`-pointer decl-init handling around the `srcIsOwningMove`
local). The plain reassignment path (`k2 = move k;`, inside `ParseAssignmentExpression`)
likely lacks the equivalent - it presumably stores the moved-in pointer value into `k2`'s
existing storage without marking `k2` newly-owning, so the scope-exit teardown logic (gated
on `IsOwning`/`IsNewAllocated`, not merely "is the type `unique`") never frees it.

## Fix direction

Find wherever a plain pointer-typed local is reassigned via `=` (the general, non-owning-
struct-value pointer reassignment branch in `ParseAssignmentExpression` - distinct from
the owning-VALUE-struct reassignment branch a few hundred lines above it, which is
already handled) and add the same `srcIsOwningMove`-shaped detection + `IsOwning` marking
the decl-init path already has, so `k2 = move k;` transfers ownership exactly like
`unique R* k2 = move k;` does. Add a regression test (this exact repro, dtor-count-based)
to Test/test_move.cb once fixed.
