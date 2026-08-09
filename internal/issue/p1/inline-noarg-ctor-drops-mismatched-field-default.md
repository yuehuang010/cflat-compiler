# A user-written no-arg ctor drops a field default that needs a type conversion

## Summary

`ParseConstructorDefinition`'s in-line field-seeding branch
(`MainListener_Aggregates.cpp:3152-3212`) stores a field default only when it already has the
destination's LLVM type after `Upconvert`:

```cpp
fieldVal = compiler->Upconvert(fieldVal, destType);
if (fieldVal->getType() == destType) { ... CreateStore ... }
```

On a mismatch the store is silently dropped and the field keeps the zero from the seeding
`CreateStore(getNullValue(...))`. The synthesized default constructor this branch stands in for
does NOT drop it (`:342-369`): it narrows a scalar with `CreateCast` (plus the existing
narrowing warning), and for a struct-typed destination it calls the field type's default
constructor. So the same declaration gives two different values depending on whether the type
has a user-written no-arg constructor.

Two measured faces, both SILENT WRONG VALUES:

| field | no ctor (synthetic) | `C() { }` | `C(int x = 3) { }` |
|-------|--------------------|-----------|--------------------|
| `u8 r = 200;` | `200` | `0` | `0` |
| `u8 r = 128;` | `128` | `0` | `0` |
| `i16 s = 40000;` | `-25536` | `0` | `0` |
| `u32 v = 4000000000;` | `4000000000` | `0` | `0` |
| `T val = 0;` in `Box<T>`, `T = SIn` | `val.v = 7` | `val.v = 0` | `val.v = 0` |

`u8 r = 127;`, `i16 s = 300;`, `float f = 1.5;` are unaffected - `Upconvert` already lands on the
destination type for those, which is why the axis reads as covered until a value that needs a
truncation is used.

## Repro

```cflat
struct C
{
    u8 r = 200;
    int p = 1;
    C(int x = 3) { }
};
extern int main() { C c; printf("r=%d p=%d\n", (int)c.r, c.p); return 0; }
```

Prints `r=0 p=1`. Delete the constructor and it prints `r=200 p=1`.

Struct-typed face:

```cflat
struct SIn { int v = 7; };
struct GB<T> { T val = 0; int p = 1; GB(int x = 3) { } };
extern int main() { GB<SIn> g; printf("val.v=%d p=%d\n", g.val.v, g.p); return 0; }
```

Prints `val.v=0`; without the constructor, `val.v=7`.

Probes: `scratch/rev3_u8_200_*.cb`, `scratch/rev3_u8_128_*.cb`, `scratch/rev3_i16_40000_*.cb`,
`scratch/rev3_u32_big_*.cb`, `scratch/rev3_genzero_*.cb` (`_none` / `_bare` / `_alldef` triples).

## History

Pre-existing for the bare `C() { }` spelling: master (a4a90a5) prints `r=0` for it and `r=200`
with no constructor. The all-defaulted-ctor fix WIDENED the exposed population - master aborts
(rc 134) on every all-defaulted-ctor declaration, so no working program regressed, but every
such type now takes this branch. That commit mirrored two of the synthetic path's arms
(`= default`, struct-typed field with no initializer) and not this third one.

## Fix direction

Mirror `MainListener_Aggregates.cpp:342-369` in the in-line branch: on
`fieldVal->getType() != destType`, take the struct arm (forceRoot exact-key `GetFunction`, then
`CreateOverloadedFunctionCall(field.TypeName, {}, true)`, else `getNullValue`) and otherwise
`CreateCast` to `destType`.

Not applied with the all-defaulted-ctor fix because it also changes values for the PRE-EXISTING
bare-`C() { }` population, which reaches `core/` and `example/`: the accept set has to be
enumerated and the 437-file differential sweep re-run, not just the new legs.

Regression legs belong beside `testNoArgCtorFieldSeeding` in `Test/test_basic.cb`, one cell per
face (`u8 r = 200;` and the generic `T val = 0;`), each with its no-ctor twin as the oracle.
