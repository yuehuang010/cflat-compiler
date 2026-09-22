Bucket: p3 (C++ interop; follow-ups left by the unwind fixes d59df39b and fix/cpp-unwind2)

# Unwind through a CFlat frame: remaining cleanup gaps

d59df39b made calls to potentially-throwing callees `invoke` with a cleanup landing pad that runs the
scope-exit destructor walk. fix/cpp-unwind2 extended it to throwing C++ constructors / assignments /
operator new, partial construction of CFlat struct fields (synthesized and user constructors, the
synthesized struct copy), `new` freeing its block on a throwing constructor, and string / closure /
pointer statement temporaries (sink arguments excluded at the call's own invoke). Still open:

1. A CFlat `extern` function called before its body is emitted is treated as a C prototype, so calls
   to it stay plain `call`.
2. Cost: in a module that imports C++, every call to a CFlat function from a frame with pending
   cleanup becomes an `invoke`, including core helpers (`string +`, `length()`). -O2 folds most of it;
   -O0 code size grows. Measure before optimizing (e.g. mark core helpers nounwind).
3. Windows not exercised: Win64 emits cleanuppad/cleanupret under __C_specific_handler; whether
   clang-cl exceptions run them, whether drop-flag branches survive funclet outlining, and how a pad
   interacts with the `program` catch-all on a hardware fault are unverified. test_cpp_interop
   returns 0 at the top of main on Windows.
4. Partial construction of ARRAYS is not tracked: `cppunw.Tx[3] arr = default;` and
   `new cppunw.Tx[3]` whose element 2 constructor throws leak element 0 (the frame's other locals are
   destroyed). Needs an index-aware pad over the element walk (EmitFixedArrayDefaultInit, the
   `new T[n]` ctor loop, fixed-array fields in the synthesized copy).
5. Field partial construction is tracked in ParseStructDefinition, EmitAggregateFieldInitialization,
   ParseConstructorDefinition and the synthesized copy only. `class` definitions
   (ParseClassDefinition), `program` definitions and the `[cpp] struct` constructor thunk
   (EmitCppStructConstructorThunk) still leak already-built fields when a later field initializer
   throws.
6. Interface method calls (the vtable path in LLVMBackend_WinRT.cpp, `builder->CreateCall`) are never
   invokes, so an exception thrown through one skips the calling frame's cleanup.
7. `new T` for a C++ class ignores a class-level `operator new` / `operator delete` (always the global
   `_Znwm` / `_ZdlPvm`), and `new T[n]` of a C++ class allocates through the CFlat `operator new`
   rather than `::operator new[]`. Not an unwind issue, found while building the fixture.

Repro base: Test/library/cpp_interop_unwind.h / .cpp and the unwind section of Test/test_cpp_interop.cb
(codes 3160-3198).
