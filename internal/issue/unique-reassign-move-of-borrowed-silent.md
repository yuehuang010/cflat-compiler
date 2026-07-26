# `unique T*` REASSIGNMENT from a move-of-borrowed '?:' join silently borrows instead of erroring

Filed 2026-07-25. Surfaced by review of commit c315ae0 ("Judge a pointer binding's ownership
by value identity, not by a sticky flag"). Pre-existing: behavior is identical on the
pre-c315ae0 baseline (c873f15). NOT a memory-safety bug - a strictness/consistency gap.

## Status

KNOWN / OPEN. Harm is a missed compile-time rejection; runtime behavior is sound (verified:
no adoption, no double free, caller keeps ownership, one destructor).

## Symptom

The same expression that is a compile error at DECLARATION is silently accepted at
REASSIGNMENT. `move` of a borrowed pointer transfers nothing; a `unique` local fed from it
provably owns nothing, which the declaration path rejects and the reassignment path accepts.

## Repro

```cflat
int g_dtor = 0;
class R { int id = default; ~R() { g_dtor = g_dtor + 1; } };

void useBorrow(R* borrowed, bool c)
{
    // Declaration form - compile error (correct, since c315ae0):
    //   unique R* b = c ? move borrowed : nullptr;
    //   -> "cannot initialize unique 'b' from a borrowed value"

    // Reassignment form - compiles silently; b borrows:
    unique R* b = nullptr;
    b = c ? move borrowed : nullptr;
    printf("b=%d\n", b != nullptr ? b->id : -1);
}

extern int main()
{
    unique R* g = new R; g->id = 42;
    useBorrow(g, true);
    printf("gid=%d dtor=%d\n", g->id, g_dtor);   // gid=42 dtor=0 here; g freed once at exit
    return 0;
}
```

Observed: compiles, exit 0, `g` intact and freed exactly once - sound but inconsistent.
Expected: the same "cannot initialize unique ... from a borrowed value" rejection the
declaration form gets (message reworded for assignment).

The direct forms are both already rejected (verified against x64/Release at c315ae0):
`unique R* x = move borrowed;` errors at declaration, and `b = move borrowed;` errors with
"cannot assign borrowed parameter 'borrowed' to unique local 'b' - the caller still owns it
and frees it on scope exit, so this would free it twice." Only the ternary-laundered
reassignment slips through.

## Root cause

Both paths now ask the same value-identity question and both correctly answer "not owning":
the `IsBorrowed` gate (c315ae0, `ParseMoveExpression` in `cflat/MainListener.h`, around the
`RegisterMovedOutPtrValue` call) keeps a move-of-borrowed out of `movedOutPtrValues_`, so
`srcIsOwnedPtrRhs` is false and neither path adopts. The difference is what follows a "no":

- The DECLARATION path has a dedicated rejection (`RejectBorrowIntoUniqueLocal`): creating a
  `unique` local from a provably non-owning value is a contradiction, so it errors.
- The REASSIGNMENT path (`srcIsOwnedPtrRhs` consumer in `cflat/MainListener.h`, the
  `unique T*` local reassignment block around line 10639) has no counterpart check. When
  adoption is not warranted it stores the pointer without taking ownership - the sound
  fallback the mixed-join fixes rely on, but here it silently accepts contradictory code.

## Fix direction

Add the declaration path's rejection to the reassignment path: when the target is a `unique`
pointer local and the RHS value is provably non-owning AND borrow-derived (a move-of-borrowed
reaching the join, or the joined value of such arms), reject via `LogErrorContext`. The
diagnostic already exists for the direct form ("cannot assign borrowed parameter ... to
unique local ... would free it twice") - reuse or generalize it for the join case rather
than minting a new message.

Wrinkle to settle first: determine whether plain borrow reassignment into a `unique` local
(`b = someBorrow;` with no move involved) is currently legal and exercised - if so, the new
error must key on move-of-borrowed provenance specifically (e.g. a borrowed-move marker
carried on the value or arm, parallel to `movedOutPtrValues_`), not on every non-owning RHS,
or it breaks legitimate code. If plain borrow reassignment into `unique` is already rejected
or unused, the check can mirror `RejectBorrowIntoUniqueLocal` directly.

Test fallout: `Test/test_move.cb` pins the current silent-borrow behavior for the mixed-join
reassignment cases added by c873f15/c315ae0 (`unique_reassign_mixed_join_borrow_value` and
siblings). Those pin MIXED joins, which must keep borrowing - the new error targets only the
non-mixed move-of-borrowed join. Add the rejected shape to
`Test/errors/err_unique_borrow_into_unique.cb`.
