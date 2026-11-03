# --symbol-dump-ir / --symbol-dump-opt ignore --platform

Summary: `cflat f.cb --platform win64 --symbol-dump-ir function:F` (and `module`) emits host IR - `target triple = "arm64-apple-macosx..."`,
Itanium-mangled C++ symbols (`_ZN8T513V10AD1Ev`) - instead of the requested target's IR. Cross-ABI IR inspection from macOS is therefore not
possible through the symbol-dump family; a review that relies on it (MS-ABI callee-destroys dtor counts) is vacuous.
Root cause (from review): SymbolQuery.cpp ~1328-1355 constructs a host backend and calls Analyze without applying the --platform target.
Expected: the dump honours --platform like a normal compile (or refuses the combination with an error naming the limitation).
Note: a full `--platform win64 -o` compile of a C++-interop file on macOS also fails (headers parsed for the host), so the dump is the only
practical cross view.
Found: mixcond3 r7 Sol review, 2026-10-30 box 7. Repro: scratch/box7/winprobe/mix3probe.cb (main checkout).
