# alignas(N) on a struct member is silently ignored by struct layout

Created: 2026-07-14 (found while fixing aligned-new-field-escape)

## Summary

`alignas(N)` on a struct/class MEMBER parses and is stored, but nothing in the
struct layout path ever reads it. The member's slot is not padded, the struct's
own alignment is not raised, and `sizeof` / `alignof` / the field offset are all
as if the `alignas` were absent. This diverges from C/C++, where `alignas` on a
member aligns that member's storage slot (and therefore raises the containing
type's alignment and size).

The clause is accepted without a diagnostic, so the user believes they got
padding they did not get - the cache-line-padding use case documented in
doc/HPC.md silently does nothing when written on a field instead of a type.

## Repro

```cflat
struct S { int a = 0; alignas(64) int b = 0; };

extern int main()
{
    S s;
    printf("sizeof=%lld alignof=%lld offset_b=%lld\n",
           (i64)sizeof(S), (i64)alignof(S), ((i64)&s.b) - ((i64)&s.a));
    return 0;
}
```

Actual:   `sizeof=8 alignof=4 offset_b=4`
Expected: `sizeof=128 alignof=64 offset_b=64` (C++ semantics)

## Root cause

`alignas` on a declaration is parsed into `DeclTypeAndValue::UserAlignValue`
(MainListener.h ParseDeclarationSpecifiers, ~line 2623), and struct fields are
stored as `std::vector<DeclTypeAndValue> StructData::StructFields` - so the value
IS carried on the field and IS serialized to the bitcode cache ("ua").

But `UserAlignValue` is only ever consumed for LOCALS and GLOBALS:

- `LLVMBackend::GetEffectiveAlignment(decl, type)` (LLVMBackend.h ~9632)
- `CreateGlobalVariable(...)`  (MainListener.h ~6794)
- `CreateLocalVariable(...)`   (MainListener.h ~6827)

`CreateDataStructure` (LLVMBackend.h ~10800) builds the LLVM struct body from the
field types and pads only the TAIL, using the STRUCT-level `alignas`
(`userAlign` -> `StructData::UserRequestedAlignment`). It never inspects
`StructFields[i].UserAlignValue`. Note the struct-level path is correct and is
what makes type-level over-alignment work everywhere.

## Fix direction

In `CreateDataStructure`, while assembling `types`:

1. Track the running natural offset. Before appending a field whose
   `UserAlignValue > 1`, round the offset up to it by inserting an explicit
   `[N x i8]` pad element (cflat builds padding explicitly - see the existing
   tail-pad and `PackBitfields`, which also rewrites field indices).
2. Fold `max(field.UserAlignValue)` into `StructData::UserRequestedAlignment` so
   the containing type's alignment and padded `sizeof` rise accordingly, and so
   the existing `GetEffectiveAlignmentForType` free/alloc routing picks it up.
3. Because inserted pad elements shift LLVM element indices, field lookup must go
   through a semantic-index -> storage-index map, exactly as `PackBitfields`
   already does for `__bfN` slots. This is the bulk of the work: every
   `CreateStructGEP(..., fieldIndex)` site must use the mapped index.

Alternative (much cheaper, if the layout work is not wanted): reject `alignas` on
a member with a `LogError` pointing at the type-level form
(`struct alignas(64) T { ... };`), so it cannot silently do nothing.
