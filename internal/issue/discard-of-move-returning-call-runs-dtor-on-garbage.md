# `_ = make();` on a move-returning POINTER function runs the destructor on garbage

Filed 2026-07-24, found while reviewing the `unique-pointer-reassign-via-move-loses-ownership`
fix (d33b9cf). Pre-existing on master, unrelated to that commit - the reviewer tripped over it
while probing discard positions and it is filed separately per the one-file-per-issue convention.

## Summary

The explicit-discard form `_ = <expr>;` exists so an owning value can be intentionally dropped
without tripping the mandatory-nodiscard error. For a `move T*` returning function it does call
a destructor, but on the WRONG address - an uninitialized/garbage pointer rather than the value
the call returned. The real object is never freed.

## Repro (verified against d33b9cf)

```cflat
struct R { int v = 0; ~R() { printf("dtor %d\n", v); } };
move R* make() { R* r = new R(); r->v = 7; return r; }
extern int main()
{
    _ = make();
    return 0;
}
```

Output:
```
dtor 1633015216         <- garbage; should be "dtor 7"
exit 0
```

The printed `v` is uninitialized stack/register content, and it varies run to run. So this is
both a leak (the real allocation is never freed) and a wild read - and, since the destructor
body then runs against that address, a wild WRITE for any destructor that assigns to a field or
frees an owning member. A `~R()` that freed a member would be freeing an arbitrary address.

Severity is higher than the exit-0 suggests: the abort only fails to fire because this `R` has
no owning members.

## Contrast - the shapes that work

`_ = move g;` on a global owning string/struct is correct (fixed in 009fcdb, `Test/test_move.cb`
covers it), and `_ = move x;` on a local is correct. It is specifically the discard of a
move-returning-POINTER CALL RESULT that misbehaves - the value being discarded is a call result
temp, not a named variable, so whatever the discard path reads to find the address is not the
call's return value.

## Fix direction

Not investigated in depth. Start at the `_ = ` discard handling in `ParseAssignmentExpression`
(the same region as the `TopLevelMoveExpression(assignCtx)` check at MainListener.h:9427, which
is the established idiom for "this RHS is an explicit move") and check what value it passes to
the destructor emission. The likely shape is that it emits a destructor call against the
discard target's storage - which for `_` is nothing/uninitialized - instead of against the RHS
call result.

The owning call result IS already ledgered by value identity in `ownedReturnTemps_`
(`LLVMBackend.h`) for the mandatory-nodiscard check, so the correct value is available at that
point: the discard should look the RHS value up in that ledger, free THAT, and consume the
entry.

Regression test: extend `Test/test_move.cb` with a dtor-count leg asserting `_ = make();` frees
exactly once and observes the correct field value. Add an owning-member variant (a struct whose
destructor frees a `string` member) to pin that it does not free a wild address.
