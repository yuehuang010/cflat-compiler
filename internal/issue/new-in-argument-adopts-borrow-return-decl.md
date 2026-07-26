# A `new` in ARGUMENT position makes a plain `T*` DECLARATION adopt a borrow return (double free)

Filed 2026-07-25. Pre-existing on `master`; independent of the call-argument owning-temp release
work (commit "Free a non-escaping owning-pointer temp passed as a call argument"), which emits
nothing at all in the repro below - see "Not caused by the argument-release path".

## Status

KNOWN / OPEN. Harm is a DOUBLE FREE (abort), not a leak.

## Symptom

A declaration initialized from a BORROW-returning call adopts the returned pointer whenever a
`new` was evaluated anywhere in the argument list. The named local then destroys a pointee the
real owner destroys again at scope exit.

## Repro

```cflat
int dtorCount = 0;
class R { int id = 7; ~R() { dtorCount = dtorCount + 1; } };
R* g_hold = nullptr;
R* retainIt(R* a, R* b) { g_hold = b; return a; }

extern int main()
{
    unique R* keep = new R();
    keep->id = 3;
    R* res = retainIt(keep, new R());   // borrow return, but a `new` ran in the arg list
    printf("res=%d dtor=%d\n", res->id, dtorCount);
    return 0;
}
```

Observed: `res=3 dtor=0`, then exit 134 (abort) - `res` and `keep` name one pointee and both are
freed. Expected: exit 0, `keep` freed once (the argument allocation is a separate matter).

Remove the `new` from the argument list (`retainIt(keep, nullptr)`) and the adoption disappears.
A nested-ternary flavour of the same shape is
`R* res = borrowIt(keep, idb(false) ? (idb(true) ? new R() : live) : nullptr);`.

## Root cause

`ParseNewExpression` sets the `lastOwningResult` side-channel
(`compiler->lastOwningResult = !isWinrtNew`, `cflat/MainListener.h`), which `ParseDeclaration`
consumes to decide whether the declared local OWNS its initializer. Nothing on the call path
clears it, so a `new` evaluated as a subexpression of the ARGUMENT list survives into the
enclosing declaration and is read as "the initializer produced an owned value" - even though the
initializer is the call's BORROW return, a completely different value.

The `unique` + ASSIGNMENT form of this bug was already fixed by switching that path to VALUE
IDENTITY (`IsOwnedNewTemp` on the RHS value) instead of the flag; `Test/test_move.cb:1483`
documents it. The plain-`T*` DECLARATION path still reads the flag.

## Not caused by the argument-release path

In the repro the callee RETAINS its `b` parameter (it stores it to a global), so
`ParameterRetainsArgument` answers "retains" and `RegisterNonEscapingOwningPtrArgs` registers
nothing. The emitted `main` contains zero `tmpptr.cleanup` blocks; both frees are `move.cleanup`
scope-exit cleanups of named locals. The same abort reproduces with any retaining callee.

## Fix direction

Give `ParseDeclaration` the same value-identity test the assignment path already uses: the
initializer is owned only when the initializer's RESULT VALUE is an owning temp
(`IsOwnedNewTemp` / an owning-return ledger entry with `IsOwningPtr`), not when the
`lastOwningResult` flag happens to be set. The flag should additionally be cleared when a call
result becomes the current expression value, so a stale `new` from an argument cannot reach any
later consumer.

Check the other `lastOwningResult` consumers for the same leak-through while doing this
(`lastMovedFromContainerSlot` and `lastAllocAlignment` are set and consumed the same way and may
have the same argument-position hazard).
