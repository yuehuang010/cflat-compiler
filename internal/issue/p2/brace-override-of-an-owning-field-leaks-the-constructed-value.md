# A brace override of an OWNING field leaks the value the constructor put there

Filed 2026-08-09 by the fix for `array-value-init-splat-shares-one-owning-seed`, which measured
it while separating the array splat's per-element leak from the per-element construction it was
adding. PRE-EXISTING and unrelated to arrays: the plain SCALAR spelling leaks identically.

Severity: a leak. No wrong value, no double free.

## Repro

```cflat
int ctors = 0;
string mk()  { ctors = ctors + 1;   string a = "he"; string b = "llo"; return a + b; }
string mk2() { ctors = ctors + 100; string a = "ov"; string b = "er";  return a + b; }
struct E { string s = mk(); int v = default; };
extern int main() { E e = { s = mk2() }; printf("s=%s ctors=%d\n", e.s.data(), ctors); return 0; }
```

-> compiles rc 0, runs rc 0, prints `s=over ctors=101`, and `leaks --atExit` reports
**2 leaks for 32 total leaked bytes**: the `mk()` buffer the field default put in `e.s` is
overwritten by the override store without being destructed first.

Measured identical on `c7d5978` and on `fix/splatseed`, in the scalar spelling and in the
one-element array spelling `E[1] e = { s = mk2() };`.

## Root cause

`EmitFieldInitializer` applies a named override with a plain store into the field slot. When the
element was default-CONSTRUCTED first (which is exactly what makes a partial list correct - see
the field-default brace-list record in `internal/fix-issue-lessons.md`), the slot already holds
an owning value, and nothing destructs it before the override lands.

## Consequence for the array splat

`fix/splatseed` makes the array value-init arm construct each slot independently, so this leak
now scales with N: `E[2] e = { s = mk2() };` leaks 4 / 64 bytes where the (aborting) pre-fix
binary leaked 1 element's worth. That is the correct per-element behaviour meeting a
pre-existing per-element leak, not a new defect - the discriminator is the scalar repro above,
which has no array in it and leaks the same 2 / 32 on both binaries.

## Fix direction

Destruct the existing field value before an override store, at the point
`EmitFieldInitializer` writes it, gated on `IsOwningValueType(field.TypeName)` /
`field.TypeName == "string"` - the same predicate pair the fixed-array borrow-clearing walk in
`LLVMBackend_CodegenHelpers.cpp` uses. The seeded-then-overridden shape is the only one that
can hold a live value at that point; a slot that was zeroed has nothing to free.
