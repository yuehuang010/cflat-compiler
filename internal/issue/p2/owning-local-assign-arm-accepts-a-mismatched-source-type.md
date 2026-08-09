# The owning-value LOCAL assign arm accepts a MISMATCHED source type and stores it anyway

Filed 2026-08-09 in review of `fix/arrslot`, which hit the same hole in its own new
fixed-array-element arm (fixed there in review by an alias-resolved type-equality check). The
whole-LOCAL arm it was modelled on has the hole too, and it is pre-existing, so it was left alone
rather than absorbed.

Severity: silent partial-width store, no diagnostic. Memory-safe in the measured cell only by
luck - the source is consumed and the destination keeps stale bytes.

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct Box { unique Res* item = nullptr; };
Box makeBox(int n) { Box b; b.item = new Res(); b.item->id = n; return b; }
struct Nest { Box inner; int tag = 0; };

extern int main()
{
    { Nest n; n.inner = makeBox(1); n.tag = 42;
      Nest m; m.tag = 7;
      m = n.inner;                     // Box value into a Nest slot - NO diagnostic
      printf("tag=%d dtor=%d\n", m.tag, dtor); }
    printf("end dtor=%d\n", dtor);
    return 0;
}
```

-> compiles 0, runs rc 0, prints `tag=7 dtor=0` then `end dtor=1`. Only the `Box`-wide prefix of
`m` is written (`m.tag` keeps its old 7), and `n.inner` is consumed. Measured identical on
`7beb979` and on `fix/arrslot`.

The ELEMENT spelling of the same mismatch (`Nest[2] arr; arr[0] = aBox;`) is REJECTED - master
reaches `cannot cast an aggregate value ...` and `fix/arrslot` keeps that after the review fix.
So the two spellings disagree on the same program.

## Root cause

The owning-value MOVE-at-reassignment arm in `MainListener_Expressions.cpp` (the
alloca/global-destination arm, ~2140) gates on
`compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName)` and destructs with
`GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName)` - two DIFFERENT type names, never
compared - then `return finishStore(toStore)`. The early return is what makes it silent: the
cast/assign diagnostic that would have fired lives downstream of the arm. The field-to-field arm
in the same function does compare the two names, which is why the field spelling is unaffected.

## Fix direction

Add the same alias-resolved equality the element arm now carries
(`ResolveTypeAlias(rightNV.TypeAndValue.TypeName) == ResolveTypeAlias(namedVar.TypeAndValue.TypeName)`)
to the local arm's condition, so a mismatch falls through to the existing diagnostic rather than
being stored. Pin it with an `expect_error` leg next to the `cannot cast an aggregate value` leg
in `Test/errors/err_move.cb`.

The DEREF-destination arm (`*pc = *pa`, same file) was the file's other open question and has now
been MEASURED: `Nest* mp = &m; *mp = n.inner;` is REJECTED with the same
`cannot cast an aggregate value ...` on `7beb979` and on `fix/arrslot` alike. It classifies with
`namedVar`'s type name on both sides, so the mismatch never satisfies its gate. Only the
whole-LOCAL arm is left.

Note on the diagnostic the fall-through reaches: its wording ("a fixed array decays to a pointer
to its first element") is about arrays and is unrelated to a struct-vs-struct mismatch. It is
correct to REJECT there, but the message misdescribes every cell in this family - worth a wording
pass whenever this is fixed.
