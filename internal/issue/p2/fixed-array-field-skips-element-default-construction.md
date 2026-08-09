# A fixed-array FIELD skips per-element default construction

Filed 2026-08-09 while fixing `fixed-array-field-brace-default-discarded`. This is the
NO-INITIALIZER / `= default` residue of that issue's family; the brace-list arm is fixed and
its issue file is deleted, this one is not and is deliberately left where it was.

Severity: silent wrong value.

## Repro

```cflat
struct E { int v = 7; };
struct S { E[2] e = default; };      // also: `E[2] e;` with no initializer at all
extern int main(){ S s; printf("%d %d\n", s.e[0].v, s.e[1].v); return 0; }
```

-> compiles rc 0, runs rc 0, prints `0 0` (expected `7 7`).

Oracle, verified independently: the same array as a LOCAL declarator is correct.
`E[2] e = default;` inside `main` prints `7 7` - that is what commit `987ae77`
("Default-construct each element of a stack fixed array declared '= default'") landed,
for the STACK spelling only.

Measured identical on `987ae77` and on `fix/fldarr`; neither touches this shape.

## Root cause

`987ae77` added `TryFoldGlobalDefaultConstruction` / `SplatConstantOverFixedArray` on the
declarator path (`MainListener_Declarations.cpp`). The field-default emitters reach
`GenerateDefaultValue(typeValue)` for the `= default` arm and `Constant::getNullValue` for
the no-initializer arm; neither walks into the ELEMENT type's default constructor for an
array-typed field. `fix/fldarr` routed only the BRACE-LIST arm of the field default into a
real array builder (`EmitFieldDefaultFixedArrayBrace`), so `= default` and the bare
no-initializer spelling still zero-fill.

## Fix direction

The array builder the brace arm now owns is the natural home: give the `= default` /
no-initializer field arms a slot of the array type and splat the element type's
default-constructed value over it, exactly as `EmitFieldDefaultArraySplat` does for the
named-list case (which already default-constructs the seed via
`CreateOverloadedFunctionCall(field.TypeName, {}, true)` before applying overrides). The
declarator path's constant fold is the cheaper half and applies when the element default is
constant-foldable; the non-constant case needs the memcpy splat.

Frozen legs pinning the current (wrong) value live in `Test/test_initializer_list.cb`:
"frozen: fixed-array FIELD still skips element defaults" and "frozen: fixed-array FIELD
sibling scalar default". Those two legs flip when this is fixed.
