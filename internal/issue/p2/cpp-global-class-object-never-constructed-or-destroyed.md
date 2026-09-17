# A global (file-scope) object of a C++ class is never constructed and never destroyed

Found 2026-09-17 while fixing internal/issue/p1/cpp-class-array-elements-never-constructed.md (macOS arm64, Release, master 2798eb1a).

## Summary

`ac.Trk g = default;` at file scope, where `Trk` is a foreign C++ class with a user-provided default constructor and destructor, emits a zero-initialized global and never calls the constructor; nothing destroys it at exit either. Same for a global array `ac.Trk[3] g = default;`. The single-object and array LOCAL forms construct and destroy correctly (the array form since fix/cpp-class-array-ctor).

Measured with the counters in Test/library/cpp_interop_arrayelem.h: ctor=0 dtor=0 on both the pre-fix and post-fix binaries. Symmetric (no construct, no destroy), so no double release, but a class whose constructor establishes an invariant (vptr, handles, allocations) is used in an invalid state: a virtual call on the global segfaults.

## Fix direction

Emit a module-level initializer (llvm.global_ctors entry or a call at the top of main) that runs the default constructor for every global of a nontrivially-constructible C++ class, and a matching llvm.global_dtors / atexit destruction if the global-storage ruling wants exit-time destruction (memory: globals follow Rust - no exit-time destruction for CFlat owning types; decide whether C++ globals follow the same ruling or C++ semantics). Until then, refuse a global of a C++ class with a nontrivial default constructor with a LogError diagnostic rather than zero-filling it.

Suggested bucket: p2 (clean compile, invalid object; needs a small ruling on exit-time destruction).
