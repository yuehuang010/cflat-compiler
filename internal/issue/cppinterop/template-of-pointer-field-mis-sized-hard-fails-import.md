# `ImVector<T*>` field mapped as an 8-byte pointer; the mismatch hard-fails the whole import

Found 2026-09-07 by the Dear ImGui headless spike. Distinct from the sibling issue: here
the field is NOT skipped, it is mapped to the wrong size, and the layout check then
rejects the import instead of the record.

## Repro

```cflat
import cpp "imgui.h";      // -i scratch/imgui_src
extern int main() { return 0; }
```

    spike1.cb(1,0): C++ record 'ImGuiPlatformIO' layout is not representable: cflat lays it out as 112 bytes, clang as 120
    FAIL

`ImGuiPlatformIO` ends with `ImVector<ImTextureData*> Textures;`. Clang: offset 104,
16 bytes (`int Size; int Capacity; ImTextureData** Data`), sizeof 120. cflat: 8 bytes.

## Root cause

The field type spelling `ImVector<ImTextureData *>` reaches the pointer-peeling path of
the C type mapper (`TryMapCxxForeignSpelling` / the field mapper in CClangExtract.cpp):
the `*` inside the template argument list is taken as an outer declarator, so the field
becomes "pointer to something" and gets 8 bytes. `ImVector<unsigned short>` (no `*`)
takes the other branch and is skipped (sibling issue), which is why this one surfaces
only for pointer element types.

Second defect, independent of the first: a record whose layout cannot be reproduced is a
LogError on the IMPORT line and fails the compile, even though nothing in the program
names the record. The plan's contract is a per-record refusal replayed at the use site
(the same way an unsupported signature is dropped with a recorded reason).

## Fix direction

1. The mapper must not peel a `*` that sits inside `<...>`: only trailing declarator
   characters after the last `>` are outer. Then the field falls into the sibling issue's
   path and is fixed together with it.
2. Turn the "layout is not representable" import-time LogError into a recorded record
   refusal: register the record as opaque (name only, no fields, no by-value use), keep
   the reason, and report it when CFlat code uses the record by value or touches a
   field. Pointers to it stay usable, which is what `ImGui::GetPlatformIO()` callers need.

Acceptance: `import cpp "imgui.h"` with an empty main compiles; a fixture with a struct
holding `std::vector<int*>` by value has the right sizeof; a fixture whose header has an
intentionally unrepresentable record compiles until the record is used by value, and then
reports the recorded reason at that line.
Related: [template-typed-field-drops-record.md].

## Status 2026-09-07 (working tree, uncommitted)

Both parts landed in the working tree. (1) `MapCTypeToTypeAndValueImpl` counts only the `*`
after the last `>` as outer declarators; `ImVector<ImTextureData *>` is no longer an 8-byte
pointer and falls into the sibling issue's blob fallback (16 bytes, layout exact). (2)
`VerifyCxxRecordLayout` returns the mismatch text instead of logging; `RegisterCRecords`
stores it in `CRecordEntry::layoutRefusal` and the record is refused per-record via
`RejectUnsupportedCxxLayout` at the use site. An empty-main `import cpp "imgui.h"` compiles.
The bitfield check compares absolute bit positions (storage slot byte offset * 8 + bit) with
clang's bit offset; the named-bitfield extraction site now fills `bitOffset` (it was 0 before,
which refused `ImFontGlyph` / `ImFontBaked` spuriously).
