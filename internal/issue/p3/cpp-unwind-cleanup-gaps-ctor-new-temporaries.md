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

Item 1 (filed, NOT reproduced on master, 2026-11-08 round 2): `new S{ z = t261.bval(1) }` with a CFlat struct S holding
`t261.Counted a` plus `int z`, no constructor (so not fresh-eligible), user `operator new(i64)` / `operator delete(void*)`,
caught in C++. master = worktree = clang++ -std=c++20 twin: caught, user delete 1, Counted live 0. No leak, no leg added.
The `cxxAllocator` branch it was filed against only covers C++ classes, and those reject named brace fields.
Separate, not a leak: a positional C++ class `new Cls{ bval(1) }` with class operator new/delete and a throwing argument
calls neither new nor delete on master (arguments evaluate before the block is allocated); clang allocates first and
deletes on the throw (new 1 / delete 1). An evaluation-order divergence, still open.

Item 2 (RULED 2026-10-20, match clang; OPEN, blocked on a ruling): the sized class `operator delete(void*, N)` is recognized
only when N is the target's size_t (`ulong` on LP64, `u64` on Win64). Unwind-path counts (`new S{ z = bval(1) }`,
user deletes for i64 / u64 / ulong): master 1/1/1, fix 0/0/1, clang 0/0/1. Fix patch (not applied): scratch/repro_keep/box12/unwnew/unwnew_item2_sizet.patch.
Blocked: (a) T313 r2 legs 25342-25343 and T320 r2 leg 3422 assert the i64 behaviour; the read-only rule keeps them unflipped.
(b) A non-size_t delete must still hide the base class delete, but clang rejects `std::unique_ptr<T>` for a class whose
only delete is (void*, long long) ("no suitable member 'operator delete'"), so leg 3422's unique<T320US> has no clang
counterpart. Needs a ruling on unique<T> over such a class before the legs are rewritten.
