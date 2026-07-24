# `unique` local assignment adopts ownership only from SYNTACTIC RHS shapes - indirect forms leak

Filed 2026-07-24, surfaced while fixing `unique-pointer-reassign-via-move-loses-ownership.md`
(fixed in d33b9cf). These shapes leaked on master before that commit too - d33b9cf neither
caused nor fixed them - but they are the direct residual of the formulation it chose, so they
belong together.

## Summary

d33b9cf made `unique T*` local reassignment adopt ownership using a deliberately SYNTACTIC test
(MainListener.h:9635-9645):

```cpp
bool srcIsOwnedPtrRhs = AsDirectNew(assignCtx) != nullptr
    || TopLevelMoveExpression(assignCtx) != nullptr
    || compiler->lastCallReturnsOwned;
```

`AsDirectNew` and `TopLevelMoveExpression` are strict single-child chain walks, so they cannot
see through any wrapper. That strictness is load-bearing and CORRECT - it is exactly what stops
a `new` nested in a call argument (`b = addr(new R());`) from marking a borrow-returning call's
result as owned, which was a double-free found in review. The cost is that an owning value
reaching the RHS through any indirection is not adopted, and leaks.

## Repros (both verified against d33b9cf: exit 0, no dtor printed)

```cflat
struct R { int v = 0; ~R() { printf("dtor\n"); } };
extern int main()
{
    bool cond = true;
    unique R* b = nullptr;
    b = cond ? new R() : nullptr;      // owning `new` inside a ternary arm - LEAKS
    return 0;
}
```

```cflat
struct R { int v = 0; ~R() { printf("dtor\n"); }  R* self() { return this; } };
move R* make() { return new R(); }
extern int main()
{
    unique R* b = nullptr;
    b = make()->self();                // owning call laundered through a borrow-returning
    return 0;                          // method - LEAKS
}
```

Harm is LEAK, not corruption. Erring toward the leak was the intended trade: the alternative
(a looser test) produced a free of a global's address.

## What IS covered, for contrast

- `b = new R();` - direct new, adopted.
- `b = move a;` - top-level move, adopted.
- `b = makeOwned();` - direct owning call, adopted via `lastCallReturnsOwned`.
- `b = (R*)make();` - cast around an owning call, adopted (the `lastCallReturnsOwned` leg
  survives the cast). Note this one LEAKED on master and is fixed by d33b9cf.
- `b = addr(new R());` - correctly NOT adopted (the `new` is an argument, not the result).

## Fix direction

The principled version is to stop asking "what does the RHS look like" and instead ask "does the
RHS's resulting VALUE carry ownership" - i.e. propagate an owning-ness bit on the value itself
through ternary arms, casts, and borrow-returning method chains, the way
`ownedReturnTemps_` already ledgers owning call results by value identity for the
mandatory-nodiscard check.

That ledger is the natural vehicle: if the RHS value is a still-unconsumed entry in
`ownedReturnTemps_`, the assignment should adopt it and consume the entry. The ternary case
additionally needs the ledger entry propagated from whichever arm produced it to the select
result (`internal/issue/nodiscard-residual-gaps.md` and the owned-pointer-temp work touch the
same propagation point).

Do NOT fix this by loosening `AsDirectNew` / `TopLevelMoveExpression` to look through wrappers -
that reintroduces the `b = addr(new R());` double free.

Ties into `internal/plan/ownership-transparent-assignment.md`.

Regression test: extend `Test/test_move.cb`'s `testUniqueLocalReassignMove` with dtor-count legs
for both shapes.
