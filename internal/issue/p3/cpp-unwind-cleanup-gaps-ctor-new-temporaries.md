Bucket: p3 (C++ interop; follow-ups left by the unwind fixes d59df39b and fix/cpp-unwind2)

# Unwind through a CFlat frame: remaining cleanup gaps

d59df39b made calls to potentially-throwing callees `invoke` with a cleanup landing pad that runs the
scope-exit destructor walk. fix/cpp-unwind2 extended it to throwing C++ constructors / assignments /
operator new, partial construction of CFlat struct fields (synthesized and user constructors, the
synthesized struct copy), `new` freeing its block on a throwing constructor, and string / closure /
pointer statement temporaries (sink arguments excluded at the call's own invoke). fix/cpp-unwind3
added index-aware partial construction of arrays (fixed-array locals and fields, positional lists,
`new T[n]` which also frees its block, the synthesized copy), field partial construction in
`class`, `program` and the `[cpp] struct` constructor thunk, and interface method calls as invokes.
Still open:

1. A CFlat `extern` function called before its body is emitted is treated as a C prototype, so calls
   to it stay plain `call`.
2. Cost: in a module that imports C++, every call to a CFlat function from a frame with pending
   cleanup becomes an `invoke`, including core helpers (`string +`, `length()`). -O2 folds most of it;
   -O0 code size grows. Measure before optimizing (e.g. mark core helpers nounwind).
3. Windows not exercised: Win64 emits cleanuppad/cleanupret under __C_specific_handler; whether
   clang-cl exceptions run them, whether drop-flag branches survive funclet outlining, and how a pad
   interacts with the `program` catch-all on a hardware fault are unverified; the array-prefix pad
   adds a destructor LOOP inside the cleanup (a runtime `new T[n]` or > 16 elements), and its calls
   carry no funclet bundle. test_cpp_interop returns 0 at the top of main on Windows.
4. `new T` for a C++ class ignores a class-level `operator new` / `operator delete` (always the global
   `_Znwm` / `_ZdlPvm`), and `new T[n]` of a C++ class allocates through the CFlat `operator new`
   rather than `::operator new[]`, while `delete` of that array frees through the C++
   `operator delete` (HeapAudit reports the CFlat block leaked: `new cppunw.Tx[3]` + `delete p`,
   measured 2026-09-22). Not an unwind issue, found while building the fixture.

Repro base: Test/library/cpp_interop_unwind.h / .cpp and the unwind section of Test/test_cpp_interop.cb
(codes 3160-3198, 3261-3281).
