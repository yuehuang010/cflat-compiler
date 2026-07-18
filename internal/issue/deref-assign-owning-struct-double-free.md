# `*pc = *pa` whole-struct deref-assign double-frees and leaks the destination

> RESOLVED 2026-07-17 - auto-move added for pointer-deref lvalues (gated on `isDerefStorage()`, which
> excludes every GEP so container element stores are untouched), verified green (test.bat 46/0, ASAN
> clean on both the raw-pointer and string-field repros). Safe to delete on commit.

Found 2026-07-16 during the `unique` code review. Pre-existing; NOT caused by `unique` - a plain
`string` field reproduces it identically. Confirmed under ASAN.

## DECISION (2026-07-17): auto-move, use-after-move THROUGH a pointer left unenforced

Recognize a pointer-deref lvalue as an owning-value destination and emit destruct-old + move +
zero-source, the same treatment `destIsStructField` / `destIsLocalOwningVar` get. Use-after-move
through a pointer is NOT enforced (there may be no name to `MarkVariableMoved`) - accepted, it
matches what the raw pointer already implies. Chosen over the reject option.

Guard: check `array<T>` / container element stores (single-index GEPs) are still EXCLUDED - those
are container-internal element stores that would double-free if this treatment touched them.

## Summary

Assigning one owning struct to another **through pointer dereference** (`*pc = *pa`) does a shallow
bitwise store. Both structs then own the same buffer, and both destructors free it. The
destination's previous value is also dropped without being destructed (a leak).

The named form `c = a;` is CORRECT - it auto-moves, with use-after-move enforced. Only the
pointer-deref lvalue escapes.

## Repro

```cflat
struct Res { int v = 0; };
struct Box { Res* p = nullptr; ~Box() { if (p != nullptr) delete p; } };

Box a = default;  a.p = new Res();
Box c = default;  c.p = new Res();
Box* pa = &a;
Box* pc = &c;
*pc = *pa;        // shallow store: both own a.p, and c's old Res is orphaned
```

Observed: `AddressSanitizer: attempting double-free`, plus the destination's old pointee leaks
(`scratch/rv_deref.cb`, `scratch/rv_arr.cb`).

**Control** (`scratch/rv_deref_ctl.cb`): a struct whose owning field is a plain `string` - no raw
pointer, no `unique` - double-frees on `*pc = *pa` the same way. This is a general owning-value-type
hole, not something `unique` introduced.

## Root cause

The assignment-site guards in `ParseAssignmentExpression` (`MainListener.h`) that handle owning
value types - the auto-move at reassignment, the destruct-old-before-overwrite, the copy rejections
- all key on `destIsStructField` (a 2-index GEP into a struct, or `IsInterfaceField`) or on
`destIsLocalOwningVar` (an alloca/global). A dereferenced pointer lvalue is neither, so none of them
fire and the store falls through to a plain shallow assign.

For the same reason the deref lvalue is not treated as a move site, so the source is never marked
moved-from and its destructor still runs.

## Fix direction

Recognize a pointer-deref lvalue as an owning-value destination alongside `destIsStructField` /
`destIsLocalOwningVar`, so it gets the same destruct-old-then-move treatment. The difficulty is that
the auto-move path also zeroes the SOURCE and calls `MarkVariableMoved(rightNV.CallerName)` - through
a deref there may be no name to mark, so use-after-move cannot be enforced the way it is for `c = a;`.

Options worth weighing:
- Emit destruct-old + move + zero-source, accepting that use-after-move through a pointer is
  unenforced (matches what the pointer already implies).
- Reject `*pc = *pa` for owning value types with a message pointing at `.copy()` or an explicit
  `move`, which is cheap and honest.

Check `array<T>` / container element stores before widening anything - the existing guards
deliberately exclude single-index GEPs because those are container-internal element stores that
would double-free if touched.
