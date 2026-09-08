# A field whose type is a class-template specialization drops the whole record

Found 2026-09-07 by the Dear ImGui headless spike (scratch/imgui_spike, imgui 1.93 WIP
334f484). Blocks the spike outright: `ImGuiIO`, `ImDrawList`, `ImDrawData`'s neighbours,
`ImFontAtlas`, `ImGuiStorage` and eight more records are all skipped, so `ImGui::GetIO()`
has no receiver type and nothing in the API is reachable.

## Repro

```cflat
import cpp "imgui.h";      // -i scratch/imgui_src
extern int main() { alias ImGuiIO io = ImGui.GetIO(); return 0; }
```

`-v` prints, for every record with such a field:

    skipping C struct 'ImGuiIO': unsupported field 'InputQueueCharacters' of type 'ImVector<unsigned short>'

Twelve records in imgui.h, all because a field is an `ImVector<T>` instantiation:

```cpp
template<typename T> struct ImVector { int Size; int Capacity; T* Data; /* methods */ };
struct ImGuiIO { /* ~200 fields */ ImVector<ImWchar> InputQueueCharacters; };
```

Two further records are skipped as a consequence: a field of a skipped record type is
"incomplete (unsized)" (`ImGuiSelectionBasicStorage::_Storage` of type `ImGuiStorage`).

## Root cause

Header extraction (`CClangExtract.cpp`, record field walk) maps field types through the
scalar/pointer/record map only. A `TemplateSpecializationType` field has no CFlat spelling
at extraction time because the specialization was never requested, so the field is
unsupported and the record is dropped (verbose-only note, no diagnostic at a use site).
The request machinery that binds `std.vector<int>` when a SIGNATURE names it
(`RequestCxxSignatureTypes`) is never applied to FIELDS.

## Fix direction

Treat a specialization-typed field like a specialization-typed signature: during
extraction of group G, request the field's spelling (layout only, `needDefinitions =
false`) in G's request group - the per-import scoping (a5b6a952) makes this cheap and
order-independent - then embed the resulting foreign record by value at the field's
offset, exactly as a nested C struct field is embedded today. Layout verification
(`FlattenCxxLayout`) must see the specialization's size and alignment. Member access on
the field (`io.Fonts->...`, `io.InputQueueCharacters.Size`) then works through the
existing foreign-class member path; methods on the embedded value need the definitions
upgrade (`RequestCxxForeignType(..., needDefinitions = true)`) on first call, which the
member path already does for pointee types.

Acceptance: the ImGui spike's `alias ImGuiIO io = ImGui.GetIO(); io.DisplaySize.x = 640;`
compiles; a fixture in Test/test_c_interop.cb with a struct holding `std::vector<int>`
by value reads its size through the field; `-v` no longer reports the twelve skips.
Related: [template-of-pointer-field-mis-sized-hard-fails-import.md].

## Status 2026-09-07 (working tree, uncommitted)

Interim fix landed in the working tree, narrower than the fix direction above: the record is
no longer dropped. Extraction now records every field's clang size, alignment and bit offset
(`RawField::sizeBytes/alignBytes/bitOffset`, cached as "sz"/"al"/"bo", header cache v33), and
`RegisterCRecords` embeds a field the mapper cannot spell as an opaque blob of that size
(`MakeOpaqueFieldBlob`: `u64/u32/u16/u8[N]` chosen by alignment). `-v` says
"embedded as 16 opaque bytes" for each. Layout stays exact, so the twelve records (and the
two unsized dependants) now register and `ImGui::GetIO()` has a receiver type.

Still open: the blob is not typed, so `io.Fonts.Size` / `io.InputQueueCharacters.Data` are
not addressable from CFlat. Requesting the specialization as a foreign type and embedding it
by value (the fix direction above) remains the real fix; the blob is the fallback for any
field type that the request path refuses.
