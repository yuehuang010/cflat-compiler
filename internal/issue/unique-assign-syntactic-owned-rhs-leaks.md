# `unique` assignment: owning value laundered through a BORROW-returning call still leaks

Filed 2026-07-24. Narrowed 2026-07-24 after the `?:` leg was fixed by the `ownedNewTemps_`
value-identity ledger, and again after the `unique` INTERFACE ternary was fixed by the
`valueElementTypeNames_` ledger; the remaining residual is the borrow-return chain.

## Status

FIXED: the `?:` leg. `unique T*` local reassignment now also adopts when the RHS VALUE is a
still-live entry in `ownedNewTemps_` (LLVMBackend.h) - a `new` result, propagated onto a `?:`
phi/select at `MainListener.h` `ParseTernaryBranches` / `ParseConditionalExpression`. Pinned by
`unique_reassign_ternary_*` in `Test/test_move.cb`.

FIXED: the `unique` INTERFACE ternary (`unique IShape k = cond ? new Sq() : nullptr;`). It was
never an ownership bug - the interface upcast keys off `rightNV.TypeAndValue.TypeName`, which a
`?:` phi does not carry, so a raw `ptr` was bitcast into `%__iface_fat_ptr`. `new` now ledgers its
concrete class by value identity (`valueElementTypeNames_`), and `UpcastTernaryPhiToInterface`
boxes each arm inside the arm's own branch, joining the fat pointers with a second phi - which
also handles arms of DIFFERENT concrete classes. Pinned by `unique_iface_ternary_*` /
`iface_ternary_*` in `Test/test_move.cb` and `Test/test_interface.cb`.

RESIDUAL: the one item below.

## Residual - owning call laundered through a borrow-returning method (LEAKS)

```cflat
struct R { int v = 0; ~R() { printf("dtor\n"); }  R* self() { return this; } };
move R* make() { return new R(); }
extern int main()
{
    unique R* b = nullptr;
    b = make()->self();                // owning temp from make() is never freed - LEAKS
    return 0;
}
```

`self()` returns a BORROW. Its result is a DIFFERENT SSA value from `make()`'s owning result, so
no value-identity ledger can see it as owning - which is the point: adopting a borrow-returning
call's result is only sound if the result provably aliases the owning temp, and nothing proves
that in general. Adopting it unconditionally would double-free (`make()`'s temp is separately
eligible for `pendingOwnedPtrTemps` cleanup). Harm here is a LEAK, and a leak is the accepted
trade; a double free is not. Leave it leaking unless an alias proof exists.

## What IS covered

- `b = new R();`, `b = move a;`, `b = makeOwned();`, `b = (R*)make();` - adopted.
- `b = cond ? new R() : nullptr;` and `b = cond ? new R() : new R();` - adopted (this fix).
- `b = addr(new R());` - correctly NOT adopted (the `new` is an ARGUMENT; the RHS result value is
  the borrow-returning call's own result). Also pinned for the ternary-in-argument shape
  (`borrowFirstResource(t3, useNew ? new Resource() : nullptr)`).

Do NOT fix any of this by loosening `AsDirectNew` / `TopLevelMoveExpression` to look through
wrappers - that reintroduces the `b = addr(new R());` double free. Ask whether the RHS's
resulting VALUE carries ownership, not what the RHS looks like.

Ties into `internal/plan/ownership-transparent-assignment.md`.
