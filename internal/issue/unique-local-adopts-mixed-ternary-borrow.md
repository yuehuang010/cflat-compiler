# A `unique T*` local adopts the borrow arm of a MIXED '?:' initializer (double free)

Filed 2026-07-25. Pre-existing on `master`; verified identical before and after the call-argument
owning-temp release work (commit "Free a non-escaping owning-pointer temp passed as a call
argument"). Both binaries abort with the same output.

## Status

KNOWN / OPEN. Harm is a DOUBLE FREE (abort), not a leak. Sibling of
`mixed-ternary-interface-fat-ptr-uaf.md` but on a different signal: that one is the interface
carve-out in the ternary JOIN, this one is a second adoption signal at the DECLARATION.

## Symptom

A `unique T*` local initialized from a '?:' whose arms mix an owning call and a live borrow adopts
the value regardless of which arm ran. When the borrow arm is selected, the local destroys a
pointee the real owner destroys again.

## Repro

```cflat
int g_dtor = 0;
class R { int id = default; ~R() { g_dtor = g_dtor + 1; } };
move R* makePtr() { R* r = new R; r->id = 7; return r; }

extern int main()
{
    unique R* owned = new R; owned->id = 42;
    bool a = true; bool b = false;
    unique R* u = a ? (b ? makePtr() : owned) : new R;
    printf("u=%d dtor=%d id=%d\n", u->id, g_dtor, owned->id);
    return 0;
}
```

Observed on master AND on the branch: `u=42 dtor=0 id=42`, then exit 134 - `u` and `owned` name one
pointee and both are freed. Expected: exit 0, one destructor.

## Root cause

The ternary JOIN is correctly classified: `PropagateTernaryOwnership` sees a mixed pointer join,
suppresses release and keeps the value out of `ownedNewTemps_`, so `IsOwnedNewTemp(rhs)` is false.

Adoption nevertheless happens because `srcIsOwnedPtrRhs` (`cflat/MainListener.h`, around the
`unique T*` local reassignment / initialization) is a DISJUNCTION with a second, purely SYNTACTIC
signal:

```cpp
bool srcIsOwnedPtrRhs = AsDirectNew(assignCtx) != nullptr
    || TopLevelMoveExpression(assignCtx) != nullptr
    || compiler->lastCallReturnsOwned          // <- fires here
    || compiler->IsOwnedNewTemp(rightNV.Primary);
```

`lastCallReturnsOwned` is set by `CreateOverloadedFunctionCall` from the resolved callee's declared
`move` / unique return type. The inner `makePtr()` arm is EMITTED even though it is not selected at
runtime... and more importantly the flag survives to the declaration regardless of which arm the
phi picks, so the value-identity leg being false is overruled. The same holds for
`srcIsOwnedForUniqueIface` a few lines above.

`lastCallReturnsOwned` is a per-call side-channel with no value identity, so it cannot express
"the value that actually reached the declaration is owned" - only "some call in this RHS returns
an owning value".

## Fix direction

Make the `unique` adoption decision purely value-identity based for pointer RHS, as the ternary
join already is: keep `AsDirectNew` / `TopLevelMoveExpression` (both are syntactic tests of the
RHS ITSELF, not of a subexpression) and replace the bare `lastCallReturnsOwned` leg with
`IsOwningPtrTempValue(rightNV.Primary)`, which covers an owning-RETURN result reaching the
declaration directly and is already the release-eligibility predicate. A suppressed mixed join
then fails it and the local borrows instead of adopting - the allocating arm leaks, which is the
accepted trade.

Audit `srcIsOwnedForUniqueIface` in the same change; it has the same `lastCallReturnsOwned` leg
and the interface join is not yet strict (see `mixed-ternary-interface-fat-ptr-uaf.md`), so the
two must be fixed together to be effective.
