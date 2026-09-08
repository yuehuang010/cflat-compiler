# Mixed-type bitfields are packed by MSVC rules; Itanium packs them into one unit

Found 2026-09-07 by the Dear ImGui headless spike (imgui_internal.h via imgui.cpp). Not blocking:
the record is refused per-record and nothing in the spike names it.

## Repro

```cpp
struct ImFontAtlasRectEntry { int TargetIndex : 20; unsigned int Generation : 10;
                              unsigned int IsUsed : 1; };
```

`-v` on `import cpp "imgui.cpp"` (macOS arm64):

    C++ struct 'ImFontAtlasRectEntry': layout is not representable: cflat lays it out as 8 bytes, clang as 4

## Root cause

`PackBitfields` (LLVMBackend_CInterop.cpp) opens a new storage unit whenever the declared type
changes (`int` -> `unsigned int`), which is the MSVC rule. The Itanium ABI (clang on macOS/Linux)
keeps packing into the current unit as long as the bits fit, regardless of the declared type, so
clang lays the three fields out in a single 4-byte unit.

## Fix direction

Make the packing rule follow the target: on an Itanium target only a bit overflow (or a `:0`
zero-width field) starts a new unit; keep the MSVC rule on Windows. `VerifyCxxRecordLayout`
already compares absolute bit positions, so a wrong rule is refused rather than mis-laid-out.

Acceptance: `ImFontAtlasRectEntry` registers on macOS; a fixture with mixed `int`/`unsigned`
bitfields reads each field back through CFlat with the same values a C++ helper reports;
Windows keeps its current layout for the same fixture.
