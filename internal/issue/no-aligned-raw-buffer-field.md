# No way to own an over-aligned raw buffer (e.g. 64-byte-aligned double[]) in a struct field

Created: 2026-07-14 (found while fixing aligned-new-field-escape)

## Summary

There is no way to make a struct OWN a raw over-aligned buffer such as a
64-byte-aligned `double[]`. The two mechanisms cflat has each fall short:

- **Type-level `alignas`** works everywhere (it rides the static type into every
  free site, as in C++) but you cannot `alignas` a primitive: `double` is 8-byte
  aligned, full stop. Wrapping it - `struct alignas(64) Chunk { double[8] v; }` -
  works, but it changes the element type, `sizeof`, and the array stride, so the
  buffer is no longer a flat `double[]` a `vectorize` loop can stream.
- **Per-site `new T[n] alignas(N)`** gives exactly the flat, over-aligned
  `double[]` that SIMD kernels want, but the alignment is deliberately NOT in the
  type, so it cannot survive an escape. Storing it in a field, returning it, or
  moving it into a `move` param are all compile errors (they used to be silent
  heap corruption - see the fixed aligned-new-field-escape).

So an over-aligned SIMD buffer must live in the local that allocated it. Any type
that wants to own one - the natural `Matrix` / `Tensor` / `FftPlan` shape, where a
long-lived object holds an aligned scratch buffer - cannot be written.

## Repro

```cflat
struct Matrix
{
    double[] data = default;              // want: 64-byte aligned, flat doubles
    ~Matrix() { delete[_] data; }
};

extern int main()
{
    Matrix m;
    m.data = new double[1024] alignas(64);   // error: cannot store an over-aligned buffer
    return 0;
}
```

The workaround (`struct alignas(64) Chunk { double[8] v; }` + `Chunk[] data`)
compiles and frees correctly, but the kernel now indexes `data[i].v[k]` instead of
`data[i]`, and `sizeof(Chunk) == 64` fixes the tiling.

## Root cause

The alignment of `new T[n] alignas(N)` is an ALLOCATION property, tracked on the
value (`NamedVariable::AllocAlignment`) and consumed by the two free sites that
can see the owning local (`EmitOwningPtrCleanup`, and `delete` in
ParseDeleteExpression). A struct field's type (`double[]`) carries no alignment,
and `alignas` on the field means "align the field's slot" in C/C++ - it says
nothing about the pointee - so it cannot be repurposed to describe the block.

## Fix direction

Introduce an allocation-alignment clause that is DISTINCT from `alignas`, and let
it appear on a pointer/array-view declaration as well as on `new`, so the two
agree and the free site can recover it:

```cflat
struct Matrix
{
    double[] data align_alloc(64) = default;   // the BLOCK this field owns is 64-aligned
    ~Matrix() { delete[_] data; }
};

m.data = new double[1024] align_alloc(64);     // checked against the field's clause
```

Then:

- store the clause on `DeclTypeAndValue` (a new field, next to `UserAlignValue`);
  it is already serialized per-field, so the bitcode cache comes along;
- a field read stamps it onto the resulting `NamedVariable::AllocAlignment`, so
  `delete data` in the destructor routes to `__delete_aligned` (this is exactly
  the machinery the aligned-new-field-escape fix already built - only the SOURCE
  of the tag changes);
- a store into such a field checks that the allocation's alignment matches;
- the same clause on a `move` parameter and on a return type would close the
  remaining two rejected escapes.

Containers (`list<T*>`) stay out of reach either way: the element free is shared
by every instantiation, so it would need the alignment in the element TYPE.

Naming is the open question - `align_alloc(N)` / `alignas_alloc(N)` / an
`aligned<T, N>` wrapper type. A wrapper type is the other design: it puts the
alignment back in the TYPE (so it escapes for free, no new tracking at all) at the
cost of a distinct type that must decay to `T[]` for kernels.
