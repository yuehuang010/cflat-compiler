# Scalar `unique IFace` field is released by nothing

## Summary

A struct field holding a boxed interface value declared `unique` - whether written directly or
reached through a generic type argument - is never released. The synthesized destructor's scalar
arm skips it, and there is no other release path, so the boxed object leaks silently.

The ARRAY form (`unique IFace f[N]`) is handled: `GetOrCreateFullDestructor`'s array arm carries
an `IsIface` flag and routes to `EmitUniqueArrayFieldRelease`, which is fat-pointer aware. Only
the scalar form is missing.

## Repro

Measured on macOS Release, `scratch/spike_iface.cb`:

```cflat
import "diagnostic/heap_audit.cb";

int g_dtor = 0;
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } ~Circle() { g_dtor = g_dtor + 1; } };

struct Holder<T> { T _v = default; };

extern int main()
{
    HeapAudit.enable();
    {
        Holder<unique IShape> h = default;
        Circle* c = new Circle();
        c->r = 3;
        h._v = c;
    }
    printf("iface: dtor=%d leaks=%u\n", g_dtor, HeapAudit.reportLeaks());
    return 0;
}
```

Observed: `iface: dtor=0 leaks=1`. Expected: `dtor=1 leaks=0`.

The scalar-pointer counterpart (`Holder<unique Payload*>`) was fixed on 2026-07-20 and measures
`dtor=1 leaks=0`; this case was left open deliberately.

## Root cause

`GetOrCreateFullDestructor` (`cflat/LLVMBackend.h` ~3079) guards the scalar arm with `f.Pointer`:

```cpp
if ((f.IsUnique || f.IsUniqueTypeArg) && f.Pointer && !f.ElemPointer && ...)
```

A boxed interface value is a `{i8*,i8*}` fat pointer, not a single-indirection pointer, so
`f.Pointer` is false and the field never enters the destructor's work list.

This fails SAFELY: the guard declines the case rather than handing a fat pointer to
`EmitUniqueFieldDelete`, which is a raw-pointer emitter. Routing it there without the interface
path would free the wrong address - a corrupting free is strictly worse than the current leak.

Note the same `f.Pointer` shape test is why `ValidateUniqueField` (`cflat/MainListener.h:6622`)
rejects a WRITTEN `unique IShape x` field with "requires a single-indirection pointer type". The
declaration-time rejection and the destructor-time skip are the same limitation seen twice.

## Fix direction

Plumb the interface path the array arm already has:

1. Give the scalar arm an `IsIface` determination equivalent to the array arm's, and route
   interface-valued fields to a fat-pointer-aware scalar emitter (release through the vtable dtor
   slot, as `EmitUniqueArrayFieldRelease` does per element).
2. Then, and only then, relax `ValidateUniqueField`'s single-indirection message for the
   interface case - the two must move together or a written `unique IShape` field becomes
   spellable while still leaking.

Do NOT widen the `f.Pointer` guard without step 1.

## Context

Filed 2026-07-20 as the remaining half of `internal/plan/unique-ownership.md` NEXT item 3.
