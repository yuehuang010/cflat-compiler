# `T[N] a = { { field = v } };` - a nested NAMED brace per array element segfaults

Filed 2026-08-21 while closing the brace-init owning-temp-field p1. **Pre-existing on master
(`b0361bb`) and unchanged by that work** - measured on both binaries.

Not a temp/ownership bug: it reproduces with a plain NAMED local as the source, so the source's
provenance is irrelevant. The fixed-array element path never reaches `EmitOneFieldInit`, so none
of that function's field-store rules (ownership, string copy, interface rebox, code-value gate)
apply to an array element either.

## Repro

```cflat
import "string.cb";
struct Tok { string text = default; };
struct Holder { Tok slot = default; };
extern int main() {
    Tok t = default; t.text = "abcdefghijklmnop" + "qrstuvwxyz";
    Holder[1] arr = { { slot = t } };          // compiles clean
    printf("out=%s\n", arr[0].slot.text.data());
    return 0;
}
```

`exit=139` (SIGSEGV), no diagnostic, on both binaries. The scalar spelling of the same store
(`Holder h = { slot = t };`) is correct and prints the text.

## Fix direction

Find the fixed-array brace path (`EmitFixedArrayBraceInit` / the `flatten` in
`MainListener_Expressions.cpp`) and establish what it does with a nested `initializerList` whose
elements are NAMED (`field = value`) over a STRUCT element type - it currently reads
`fi->assignmentExpression(0)` per element, which a nested named brace does not have. Either route
the element through `EmitFieldInitializer` (which is what the scalar spelling uses and what would
give the element every field-store rule for free) or reject the spelling with a real diagnostic.
Coverage: `T[N]`, `T[N][M]`, the inferred `T[]` view spelling, a global array, and a struct FIELD
of array type - measure each before fixing.
