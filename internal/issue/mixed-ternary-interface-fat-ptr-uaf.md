# A mixed-ownership `?:` over an INTERFACE fat pointer adopts a borrow (use-after-free)

Filed 2026-07-25. Pre-existing on `master`; identical behaviour before and after the
call-argument owning-temp release work (commit "Free a non-escaping owning-pointer temp passed as
a call argument"), which deliberately carved interfaces out rather than introducing this.

## Status

KNOWN / OPEN. Harm is a USE-AFTER-FREE plus a double free, not a leak. Fixing it requires
reworking `Test/test_move.cb`'s `testUniqueInterfaceTernary` expectations, which is why it was not
folded into the pointer-side fix.

## Symptom

A `?:` whose arms mix an owning value and a live BORROW propagates the owning bit onto the joined
value regardless of which arm the condition selects. When the BORROW arm is taken, a `unique`
local adopts a fat pointer someone else owns, destroys it at scope exit, and the real owner then
destroys it again.

## Repro

```cflat
interface IShapeMove { int area(); };
int dtorCount = 0;
class SqMove : IShapeMove
{
    int s = 3;
    SqMove() { }
    int area() { return s * s; }
    ~SqMove() { dtorCount = dtorCount + 1; }
};
bool identityBool(bool b) { return b; }
move IShapeMove makeShapeMove() { return new SqMove(); }

extern int main()
{
    unique IShapeMove owner = new SqMove();
    IShapeMove borrowed = owner;
    {
        unique IShapeMove k = identityBool(false) ? makeShapeMove() : borrowed;
        printf("area=%d\n", k.area());
    }
    printf("after inner dtorCount=%d (expect 0)\n", dtorCount);
    printf("owner still alive area=%d\n", owner.area());
    return 0;
}
```

Observed: `dtorCount=1` after the inner scope (the borrow was destroyed), then garbage from
`owner.area()`, then SIGABRT (exit 134) when `owner` is destroyed a second time. Expected
`dtorCount=0` and `area=9`. Same output on `master` and on the branch.

## Root cause

`PropagateTernaryOwnership` (`cflat/LLVMBackend.h`) applies the strict "every arm must be owning
or null" rule only when the joined value `isPointerTy()`. An interface value is a `{i8*,i8*}` fat
pointer - a STRUCT type - so it takes the older either-arm branch: whichever arm carries an
owning-return ledger entry stamps the join, and the entry then drives adoption at
`unique IShapeMove k = ...` and the scope-exit release in `EmitOwningInterfaceCleanup`.

This is the same unsoundness the raw-pointer path had before the strict rule was introduced - the
ternary's owning bit describes an arm that may not be the one that ran.

## Fix direction

Extend the strict all-arms rule to interface fat pointers: treat an arm as joinable only when it
is an owning temp (owning-return ledger or owning-`new` ledger) or a null/zeroinitializer
constant, and suppress ADOPTION as well as release for a mixed join, keeping the entry visible to
the no-discard check exactly as the pointer path now does (`CallerReleaseSuppressed`).

Two things to plan for:

- `Test/test_move.cb`'s `testUniqueInterfaceTernary` pins the current either-arm adoption
  behaviour across 13 assertions. Narrowing the rule requires REWORKING those expectations (the
  mixed cases become "not adopted, the allocating arm leaks"), not reverting the rule. Do not
  weaken the rule to keep the old counts.
- The other owning-value struct joins (`string`, owning-value structs, closure fat pointers) reach
  the same either-arm branch and should be audited for the same shape at the same time. Their
  release paths differ (`FlushOwnedStringTemps` / `FlushOwnedStructTemps`), so each needs its own
  "is this arm actually owning" test rather than a shared pointer-only gate.

A leak on the mixed join is the accepted trade, exactly as on the pointer path; a use-after-free
is not.
