# A `move` of a BORROWED pointer as a '?:' arm adopts the borrow (double free)

Filed 2026-07-25. Pre-existing on `master`; unchanged by the mixed-'?:'-pointer-join fix
(commit "Stop a mixed '?:' pointer join from laundering ownership into its receiver"), which
made the join classifier value-identity based but did not teach it borrow PROVENANCE.

## Status

KNOWN / OPEN. Harm is a double free plus a use-after-free of the caller's live object, not a leak.

## Symptom

`move p` where `p` is a BORROWED pointer (a plain, non-`move` parameter the caller still owns)
transfers nothing - nulling the callee's copy leaves the caller the sole owner. As a '?:' arm it
is nevertheless scored OWNING, so the join is uniform, the receiver adopts it, and the pointee is
destroyed once by the receiver and again by the caller.

## Repro

```cflat
int g_dtor = 0;
class R { int id = default; ~R() { g_dtor = g_dtor + 1; } };
move R* makeR() { R* r = new R; r->id = 7; return r; }

void useBorrow(R* borrowed, bool c)
{
    // move of a BORROWED param transfers nothing; joined with a genuinely owning arm.
    R* v = c ? move borrowed : makeR();
    printf("  inner v=%d dtor=%d\n", v->id, g_dtor);
}

extern int main()
{
    {
        unique R* g = new R; g->id = 42;
        useBorrow(g, true);
        printf("  after call dtor=%d gid=%d\n", g_dtor, g->id);
    }
    printf("after scope dtor=%d\n", g_dtor);
    return 0;
}
```

Observed on `master` and on the fix branch: `dtor=1` and a garbage `gid` immediately after the
call, then exit 134 at scope exit. Expected: exit 0, one destructor, `gid=42` intact.

## Root cause

`ParseMoveExpression` (`cflat/MainListener.h`) nulls the source and reports the transfer through
`lastOwningResult` plus, since the join fix, a value-identity entry via `RegisterMovedOutPtrValue`.
Neither channel records that the moved-out value was BORROWED. `ParseMoveExpression` does compute
the provenance - it copies `argNV.IsBorrowed` / `BorrowedOrigin` onto the returned `NamedVariable`
precisely so a store into a `unique` FIELD can be rejected - but the ternary join sees only an
`llvm::Value*`, so `TernaryArmJoinsOwning` answers "owning" for a borrow.

The declaration path already gets this right by a different route: `ParseDeclaration` consults
`srcIsBorrowed` before adopting (`cflat/MainListener.h`, the `lastOwningResult` block that calls
`RejectBorrowIntoUniqueLocal`), which is why `unique R* x = move borrowed;` is a compile error
while the same move laundered through a '?:' is not.

## Fix direction

Carry the borrow provenance into the value-identity ledger rather than re-deriving it from syntax:
do not register a moved-out pointer in `movedOutPtrValues_` when `argNV.IsBorrowed` is set (or
record the flag alongside the value and have `TernaryArmJoinsOwning` reject borrowed entries).
A borrowed arm then fails join-eligibility, the join is classified MIXED, release is suppressed
and the sticky channels are cleared - so the receiver borrows instead of adopting, exactly as the
mixed-join path already handles `cond ? makePtr() : liveBorrow`.

Note the knock-on: with the receiver borrowing, a `unique` receiver in this shape becomes a
`cannot initialize unique ... from a borrowed value` compile error, matching the direct
`unique R* x = move borrowed;` spelling. That is the intended outcome, but it is a
behaviour change worth landing on its own rather than folded into an unrelated fix.
