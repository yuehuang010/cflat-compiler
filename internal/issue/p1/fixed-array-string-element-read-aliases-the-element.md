# Reading a fixed-array `string` element into a local aliases the element's buffer

## Summary

`string q = dst[0];` over a `string[N] dst` shallow-copies the element's `{ptr,len}` pair,
including the runtime owned bit, into the new local. Both the local and the element then free the
same buffer at scope exit: rc 133 (`malloc: pointer being freed was not allocated`).

The STORE side of this family was fixed in 9d1a731 (`dst[0] = t` and `string[2] dst = { t }` now
deep-copy). The READ side was untouched and is the next domino.

## Repro

```cflat
extern int main()
{
    string[2] dst;
    dst[0] = "ab" + "cd";
    string q = dst[0];              // shallow: q and dst[0] share the buffer
    printf("q=%d\n", q == "abcd" ? 1 : 0);
    return 0;                       // rc 133 at teardown
}
```

Prints `q=1`, exits 133. Identical with a named local as the element source
(`string t = "ab" + "cd"; dst[0] = t; string q = dst[0];`).

Both spellings above ALSO fail on master (3252c01), so this is pre-existing, not a regression.
What 9d1a731 changed is the reach: with a struct-FIELD source
(`B b; b.s = "ab" + "cd"; dst[0] = b.s; string q = dst[0];`) master exited 0, because the element
merely aliased `b.s` and the array-level borrow taint suppressed the element teardown. Now that the
element genuinely owns an independent buffer, that spelling exits 133 too - the fix converting an
accidental non-owner into a real owner, which is correct per
`internal/plan/ownership-transparent-assignment.md`.

## Root cause

The declaration-initializer path has dedicated deep-copy legs for a string LOCAL source and a
string FIELD source, but a fixed-array ELEMENT source is neither: its `NamedVariable` has an empty
`FieldName`, a `CallerName` naming the ARRAY, and a `Storage` that is a two-index GEP over
`[N x %string]`. It matches no existing gate, so the plain aggregate store runs.

IR (`--out-lli`) for the repro shows the copy is a bare `extractvalue`/`store` pair into `%q` with
no `EmitOwnedStringDeepCopy`, and the teardown block calls `string.dtor` on `%q` and then on `%dst`
(elem 0's GEP folds to the array alloca) over the same pointer. `string.dtor` nulls only its own
`self`, so the second call re-frees.

## Fix direction

Mirror the store-side arm landed in 9d1a731, on the read side:

- In the declaration-initializer string path (`MainListener_Declarations.cpp`, around the existing
  `EmitOwnedStringDeepCopy` call at ~4159/5180), recognise a source whose `Storage` is a two-index
  GEP over an array type with `IsElementAccess` set, and deep-copy it exactly as a field source is
  deep-copied.
- Gate on the REPRESENTATION (`value->getType() == StructType::getTypeByName(context, "string")`),
  not on `TypeAndValue.TypeName` - an element read off a temp-producing expression carries no
  TypeName, the same trap that cost the store-side fix a cell.
- The sibling read positions share the shape and are ALL broken on master and after 9d1a731
  (measured): `string q = dst[0];` rc 133, `q = dst[0];` into an existing local rc 133, and
  `return dst[0];` rc 133 AND `r=0` (the returned value reads back wrong, not merely double-freed).
  Fix them together; the return position is the one with a wrong-value symptom.

## Regression test

Extend `Test/test_move.cb` section 16/17 (`testOwningAssignIntoFixedArrayElement`) with read legs
next to the store legs already there; assert `q.data() != dst[0].data()` for buffer distinctness,
as the store legs do. No new test file.
