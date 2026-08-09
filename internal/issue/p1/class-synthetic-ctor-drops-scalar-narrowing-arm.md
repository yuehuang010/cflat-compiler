# A `class`'s synthesized default ctor has no scalar-narrowing arm, so it emits type-mismatched IR

## Summary

`ParseClassDefinition`'s synthesized default constructor
(`MainListener_Aggregates.cpp:2786-2800`) runs `Upconvert` and then handles a type mismatch ONLY
when the destination is a struct:

```cpp
rvalue = compiler->Upconvert(rvalue, destType);
if (rvalue->getType() != destType && destType->isStructTy()) { ... }
structVal = compiler->CreateInsertValue(structVal, rvalue, structIndex);
```

`ParseStructDefinition:342-369` has an `else` arm for the SCALAR case: a narrowing
`compiler->LogWarning(...)` plus `compiler->CreateCast(rvalue, destType)`. The class emitter has
no such arm, so a mismatched scalar reaches `CreateInsertValue` unconverted and the constructor
returns a struct constant whose element type does not match the struct type - `%C = type { i8,
i32 }` but `ret %C { i16 200, i32 1 }`. The value that comes out is whatever the backend makes of
the reinterpretation, and for an int-from-float default it is plainly wrong.

## Repro

```cflat
class C { int i = 3.7; int p = 1; };
extern int main() { C c; printf("i=%d p=%d\n", c.i, c.p); return 0; }
```

Measured (macOS arm64 Release, worktree `fix/seedcast`, both before and after the inline-seeding
fix):

| declaration | `struct C` (oracle) | `class C` |
|-------------|--------------------|-----------|
| `int i = 3.7;` | `3` | `-1717986918` |
| `u8 r = 200;` | `200` (warns) | `200` (no warning, malformed IR) |
| `i16 s = 40000;` | `-25536` (warns) | `-25536` (no warning, malformed IR) |
| `bool b = 5;` | `1` (warns) | `1` (no warning, malformed IR) |
| `char ch = 321;` | `65` (warns) | `65` (no warning, malformed IR) |

Only the float-to-int cell produces a visibly wrong value; the integer cells happen to agree
because the mismatched constant's low bits are the truncation the cast would have produced. All
of them emit IR that would not survive a strict verifier check.

Probes: `scratch/au_cls_inti37.cb`, `scratch/au_cls_i16s40000.cb`, `scratch/au_cls_boolb5.cb`,
`scratch/au_cls_charch321.cb`, `scratch/au_cls_u32v4000000000.cb` and the `sc_*_none` struct
twins in the same directory.

## Root cause

The scalar `else` branch of `ParseStructDefinition:360-368` was never copied into
`ParseClassDefinition`. The class emitter is otherwise an exact copy of the struct one.

## Fix direction

Add the same `else` arm (narrowing `LogWarning` + `CreateCast(rvalue, destType)`) to
`MainListener_Aggregates.cpp:2789`, i.e. restructure

```cpp
if (rvalue->getType() != destType && destType->isStructTy())
```

into the struct emitter's nested `if (rvalue->getType() != destType) { if (isStructTy) ... else
... }` shape.

## Why it was not fixed with the inline-seeding fix

Filed while auditing the six field-seeding sites for
`inline-noarg-ctor-drops-mismatched-field-default`. That fix's accept set covers types with a
user-written no-arg or all-defaulted constructor; this site is the population with NO
constructor, a strictly larger set (every `class` in the repo), so it needs its own differential
sweep. `ParseProgramDefinition` has the identical missing arm - recorded in
`program-field-no-initializer-skips-default-ctor.md`.
