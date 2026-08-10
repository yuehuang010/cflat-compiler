# `for (Box b in coll)` aliases an owning-POINTER struct element and double-frees it

Filed 2026-08-10 from the fix/forinstr round. Pre-existing on `01853aa` and unchanged by that
round's fix, which made the fixed-array leg a BORROW by clearing owned bits - a `unique T*`
field has no owned bit, so nothing was cleared and this cell is untouched. It is filed
separately because it is NOT specific to the fixed-array leg: the container leg, whose
`alias T get` is the oracle the fix matched, fails identically.

Severity: unconditional double free (rc 133) from a plain read-only loop.

## Repro

Both spellings, measured rc 133 on `01853aa` and on fix/forinstr:

```cflat
int dtorCount = 0;
struct Res { int id = 0; ~Res() { dtorCount = dtorCount + 1; } };
struct Box { unique Res* p = nullptr; };

extern int main()
{
    int n = 0;
    {
        Box[2] arr;                       // container spelling: list<Box> l; l.add(a); ...
        arr[0].p = new Res(); arr[0].p->id = 7;
        arr[1].p = new Res(); arr[1].p->id = 9;
        for (Box b in arr) n += b.p->id;  // aliases each element's `unique Res*`
    }
    printf("n=%d dtor=%d\n", n, dtorCount);   // never reached: rc 133 at scope exit
    return 0;
}
```

Measured (scratch/fi_a_box.cb, scratch/fi_x_boxlist.cb):

| spelling | 01853aa | fix/forinstr |
|---|---|---|
| `Box[2]` fixed array | rc 133, no output | rc 133, no output |
| `list<Box>` container | rc 133, no output | rc 133, no output |

The by-value DECL spelling is correct on both (scratch/fi_x_boxdecl.cb):
`Box b = arr[0];` MOVES - `after-decl dtor=1 arrnull=1`, total `dtor=2`, rc 0.

A MIXED element - a struct with BOTH a `string` field and a `unique T*` field - is this same cell,
not a new failure mode (scratch/rev_mixed.cb, scratch/rev_mixedstr.cb): rc 133 on both binaries,
from the pointer alone. Clearing the owned bits does fix the string half of it - the pre-teardown
read is `n=124` on `01853aa` (the second element's string already corrupted) and `n=324` on
fix/forinstr (both strings intact) - so only the pointer half is left for this issue.

## Root cause

The range-`for` loop variable's alloca is hoisted once and its destructor is emitted ONCE, in
`forRangeResume`. For a `string` element (and for a struct's `string` / owning-value FIELDS) the
element can be handed over as a BORROW because the representation carries an owned bit, which
`ClearStringOwnedBit` / `ClearStructOwnedBits` clear - the once-only dtor then no-ops. A
`unique T*` field has no such bit: `ClearStructOwnedBits` explicitly skips `f.Pointer` fields, so
the loop variable receives a live copy of the pointer and the synthesized `Box` destructor at
`forRangeResume` deletes an object the array (or the list) still owns.

So the element-borrow mechanism the rest of the family relies on has NO representation for an
owning-pointer field, in either the fixed-array leg or the container `alias T get` leg.

## Fix direction

Three candidates, none cheap enough to fold into the borrow-bit fix:

1. Give the loop variable a per-iteration destruct scope (emit its dtor at the end of the loop
   BODY rather than at `forRangeResume`) and MOVE the element in per iteration - this is
   option 2 of the deleted `for-in-over-a-string-array-aliases-each-element.md`, and it changes
   loop-variable lifetime for every element type, so it needs its own accept-set sweep.
2. Suppress the loop variable's destructor entirely when the element type is an owning-pointer
   struct, making it a genuine non-owning view. Needs a per-variable "never destruct" flag that
   survives the loop's scope teardown.
3. Reject `for (T x in coll)` at compile time when `T` has an owning-pointer field and the loop
   variable is bound by value, with a located error pointing at the field. Least code, and an
   always-double-freeing program is what is being rejected - but check `example/` and `core/`
   first, and check whether an `alias T` loop-variable spelling exists to recommend.

Whatever is chosen must be applied to BOTH legs (the `isFixedArray` branch of
`MainListener_Statements.cpp` and the container `alias T get` return path) - they fail
identically today and a fix to one alone would split them.
