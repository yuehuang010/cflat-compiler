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

1-2. (fixed de195f61) extern CFlat bodies defined later now invoke; core functions proven non-unwinding
   at --init stay plain calls (dropped when the program overrides a C extern they rely on).
3. Windows not exercised: Win64 emits cleanuppad/cleanupret under __C_specific_handler; whether
   clang-cl exceptions run them, whether drop-flag branches survive funclet outlining, and how a pad
   interacts with the `program` catch-all on a hardware fault are unverified; the array-prefix pad
   adds a destructor LOOP inside the cleanup (a runtime `new T[n]` or > 16 elements), and its calls
   carry no funclet bundle. test_cpp_interop returns 0 at the top of main on Windows.

Repro base: Test/library/cpp_interop_unwind.h / .cpp and the unwind section of Test/test_cpp_interop.cb
(codes 3160-3198, 3261-3281).

## 2026-10-16 (T313 reviews 2-3): brace `new` of a non-fresh struct gets no unwind note

`new S{ z = rv.bval(1) }` where S holds a C++ class plus an int and is not eligible for the fresh in-place brace path:
when the element initializer throws, no UserNewHeap unwind entry is recorded, so the block leaks (user delete count 0,
master same). The tag form, `new T(args)` and fresh-eligible `new T{...}` are covered by T313 (EmitUserNewHeapFree).
Also from T313 review 3, for a ruling only: the sized class `operator delete(void*, N)` is recognized for ANY non-pointer
64-bit integer N (i64 or u64); clang accepts only std::size_t (long long / unsigned long long free nothing there).
