# Storing into a `T[]` VIEW element leaks the old element

Filed 2026-08-09 by `fix/arrslot`, which measured this as the boundary of its own accept set.

Severity: double free (abort, rc 133) for an INDIRECT source; leak (no abort) for a named one.
Re-measured 2026-08-09 by `fix/bvfield` - see "Worse than filed" below.

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

## Worse than filed (re-measured 2026-08-09, `fix/bvfield`)

The filed repro uses a NAMED local source (`v[0] = a`), which reaches a consume arm and only
orphans the old element - a leak, as recorded. An INDIRECT source (a field path or any other
lvalue with no name) reaches NO consume arm at all: the plain store aliases the source's owning
bits, and both the element and the source free them.

```cflat
// scratch/bv_c4 - LOCAL struct field source
extern int main() {
    { Wrap w; w.b = umk(3); UBox[] v = new UBox[2]; v[0] = w.b;
      printf("v=%d\n", v[0].item->id); }
    printf("d=%d\n", dtor); return 0; }
```

-> compiles 0, prints `v=3`, then ABORTS (rc 133). Measured identical on `6c2302c` and on
`fix/bvfield`. The named-source control (`v[0] = b`, `scratch/bv_c5`) is rc 0 / `d=1` on both, so
the difference is the source's indirectness, not the view destination alone.

Consequence for the fix: the view-element destination needs the owning-value CONSUME arm the
fixed-array element destination has (`MainListener_Expressions.cpp` ~2205), not only a drop-old.
When it gets one, `fix/bvfield`'s `RejectConsumeOfBorrowedByValueParamField` must be installed at
that arm too - `int f(Wrap w) { UBox[] v = new UBox[2]; v[0] = w.b; ... }` (`scratch/bv_19`) is
the borrowed-by-value-parameter shape, and it is the one store spelling of `w.b` that this round
could not close because there is no arm to hang the guard on.
