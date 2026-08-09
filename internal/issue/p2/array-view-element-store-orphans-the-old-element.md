# Storing into a `T[]` VIEW element leaks the old element

Filed 2026-08-09 by `fix/arrslot`, which measured this as the boundary of its own accept set.

Severity: leak (no abort).

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct Box { unique Res* item = nullptr; };
Box makeBox(int n) { Box b; b.item = new Res(); b.item->id = n; return b; }

extern int main()
{
    { Box a = makeBox(5);
      Box[2] base; base[0] = makeBox(1);
      Box[] v = base;
      v[0] = a; }                       // old base[0] is never destructed
    printf("end dtor=%d\n", dtor);      // prints 1; two Res were allocated
    return 0;
}
```

-> compiles 0, runs rc 0, prints `end dtor=1` for two allocations. Measured identical on
`7beb979` and on `fix/arrslot`.

## Root cause

A view subscript is a SINGLE-index GEP over the element type (verified in `--no-opt` IR:
`getelementptr %Box, ptr %5, i64 0`), so it lands in Part 6's container-slot arm
(`MainListener_Expressions.cpp`, `destIsElemSlot`). That arm transfers correctly - the source IS
consumed, which is why this is a leak and not a double free - but it deliberately does NOT
drop-old, because a container only ever writes into a slot it has already released
(`list.set` -> `_releaseAt` then `_placeAt`); a drop-old there double-destructs.

A `T[]` view over a fixed array is the case where that assumption is false: the viewed slots are
LIVE. The fixed-array spelling of the same store (`base[0] = a`) is a TWO-index GEP and was
fixed by `fix/arrslot` with its own accept set; the view spelling has the container's GEP shape
and cannot be separated from a real container slot by the GEP alone.

## Fix direction

`ownership-transparent-assignment.md`'s LOAD-BEARING INVARIANT forbids widening the single-index
gate (dictionary rehash and list `sort`/`_partition` bit-shuffles depend on it), so the fix is
NOT a wider gate - it needs a positive signal that the destination is a user `T[]` view rather
than a container's raw `T*` buffer. `namedVar.TypeAndValue.IsArrayView` is already cleared by the
time the ELEMENT NamedVariable is built (the Part 6 gate tests it and it is false here), so the
first step is to carry the base binding's view-ness onto the element access, then give the
view-element case the drop-old the fixed-array case now has. Re-probe both invariant paths on any
change here.
