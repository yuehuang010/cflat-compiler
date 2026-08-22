# Explicit `move` into an INDIRECT lvalue copies instead of transferring, orphaning the source buffer

## Summary

`dest = move src` transfers correctly when `dest` is a named local/global, but when `dest` is an
INDIRECT lvalue (a pointer deref, or now an `alias` call result) the assignment deep-COPIES the
source. `move` has already zeroed the source slot by then, so the value that was moved out is
copied and the original buffer is orphaned - one leak per assignment.

Pre-existing: reproduced on the unrelated `ad95b47` build with no `alias` in the program. The
`alias`-return-lvalue work only made a second spelling (`rows[0] = move t`) able to reach it.

## Repro

```cflat
struct Row { string text; };
extern int main()
{
    Row a = default; a.text = "old".copy();
    Row t = default; t.text = "new".copy();
    Row* p = &a;
    *p = move t;                       // leaks the "new" buffer (16 bytes)
    printf("row=%s\n", p->text.data());
    return 0;
}
```

`leaks --atExit`: 1 leak / 16 bytes. `a = move t;` (named destination, same types) is 0 leaks.
`rows[0] = move t;` and `rows.get(0) = move t;` on a `list<Row>` leak identically.

## Root cause

The deref/indirect arm in `MainListener_Expressions.cpp` (the "Owning-value MOVE through a
pointer-deref destination" arm, ~:2955) classifies the source with
`ClassifyOwningAssignSource(..., rightNV.TypeAndValue.IsMove, ...)` and emits `Row.copy.synth`
for it, i.e. it takes the Copy branch. The named-local arm (~:2749) has the same gate and takes
the Move branch on the same program, so the two arms disagree about the same source. The emitted
IR shows the order `load t` -> `store zeroinitializer, ptr %t` (the `move` expression's own
source-nulling) -> `copy.synth` -> drop-old -> store, so the copied-from value is the last
reference to the orphaned buffer.

## Fix direction

Find why the two arms classify the same `move` source differently (the deref arm sees the source
as non-move where the named-local arm sees it as move) and make the indirect arm take the same
Move branch: store `right` unchanged, skip the copy, and leave the already-zeroed source alone.
Guard against re-zeroing a source the `move` expression has already cleared. Regression legs
belong in `Test/test_list_ownership.cb` next to `testAliasLvalueAssignDropsOldOwner` - assert the
HeapAudit leak count, not just the stored value, since both value assertions pass while leaking.
