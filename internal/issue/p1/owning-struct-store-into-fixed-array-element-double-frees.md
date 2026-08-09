# Storing an owning struct into a FIXED-ARRAY element leaks the old element and double-frees

Filed 2026-08-09 by `fix/owncopy` (the indirect-SOURCE fix), which measured the destination-side
twin of its own family and deliberately left it out: this one is broken for EVERY source shape,
including a plain named local, so it is a destination-side gap with its own accept set.

Prelude: `Res` with a dtor counter, `Box { unique Res* item; }`, `makeBox`, `Wrap { Box b; }`.

## Repros (both: compile 0, run rc 133; identical on the pre and post `fix/owncopy` binaries)

```cflat
// (a) NAMED source - no indirection anywhere
Box a = makeBox(5);
Box[2] dst; dst[0] = makeBox(1);
dst[0] = a;             // rc 133; "dtor=0" first - the old element was never destructed

// (b) indirect source
Wrap w; w.b = makeBox(5);
Box[2] dst; dst[0] = makeBox(1);
dst[0] = w.b;           // rc 133, same shape
```

A call-result source is fine (`dst[0] = makeBox(5);` runs clean but still LEAKS the old element -
measured `end dtor=1` for two allocations).

## Root cause (hypothesis)

A subscript of a FIXED array produces a TWO-index GEP over the `[N x T]` type, so it is neither
`destIsStructField` (which requires a struct source element type) nor `destIsLocalOwningVar`
(alloca/global) nor `destIsElemSlot` (Part 6's SINGLE-index container-buffer GEP,
`MainListener_Expressions.cpp` ~2420). Every owning-store arm therefore skips it and it falls to
the plain bit store: the old element is never dropped and the source is never consumed.

## Why it is not the container slot case

Part 6 deliberately does NOT drop-old, because a container only ever writes into a slot it has
already released. A FIXED array's elements are live default-constructed values, so this
destination needs drop-old - a different decision, which is why it needs its own accept set
(`ownership-transparent-assignment.md`'s LOAD-BEARING INVARIANT forbids widening the
single-index-GEP gate to cover it).

## Related

The SOURCE-side twin (indirect deref/field/element sources) was fixed on 2026-08-09 - see the
landed record at the bottom of `internal/fix-issue-lessons.md`.
`internal/plan/ownership-transparent-assignment.md` Part 6.
