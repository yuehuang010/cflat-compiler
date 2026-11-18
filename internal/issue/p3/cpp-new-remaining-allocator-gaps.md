Bucket: p3 (C++ interop; follow-ups left by fix/cpp-class-operator-new, 2026-09-23)

# C++ `new` / `delete`: remaining gaps after class-level operator pairing

Measured before and after fix/cpp-class-operator-new (Test/library/cpp_interop_opnew.h,
legs 3350-3366 of Test/test_cpp_interop.cb):

1. (fixed 2026-09-25, a31f4a72 + 843a9f8c) `(new T(x))->m()` is freed when the linked body of `m`
   provably does not retain `this`, in any function including returns and generic instantiations,
   and on the unwind path. Virtual / unlinked / escaping methods keep the bounded leak. Remaining
   dangling-address shapes were meant for a p2 file that was never created (none in git history,
   checked 2026-11-06); re-probe them before relying on this list.
2. (not reproduced 2026-09-24; re-probed 2026-11-09: a `unique` struct field of a C++ class, assigned `new`
   twice, gives exact counts, 121 = clang twin, on master and fix)
3. (fixed 767ae268) runtime-count delete of a polymorphic C++ raw pointer.
4. (fixed 767ae268) private/protected class operator new/delete refused. Placement, aligned and
   destroying forms are still not exported.
5. Windows: the MS-ABI names for `operator new[]` / `delete[]` (`??_U@YAPEAX_K@Z`,
   `??_V@YAXPEAX@Z` and the aligned forms) are written but unexercised on macOS.

Fix direction per item: (1) register the temporary in the statement-temp destruction list with
the paired delete; (2) treat a `new` expression as owning at the field-store site; (3) loop the
deleting destructor over the runtime count; (4) refuse `new T` when the selected operator is
inaccessible; (5) run test_cpp_interop on Windows.

## From T257 review 1 (pre-existing on master; repros scratch/repro_keep/t257_rev/, probe script beside them)
- `new DM(v)` from a `volatile DM&` source is accepted (clang refuses: no volatile copy ctor), dm_vol.
- `new CVA(4)` through a `using` alias of a class with a converting ctor fails "has no imported constructors"; clang 8, cv_alias.
- `new DM(vref())` from a `volatile DM&` source is accepted by a stack declaration too (`DM q = vref();`), so the
  gap is general volatile-copy acceptance, not new-specific; CFlat drops volatile by design. Needs a ruling.
- Open shapes after the 2026-11-09 elision of `new T(f())` (f returns T by value). Ternary arms of two
  same-class prvalues are fixed (legs 4007-4008). Still open, clang twin in brackets:
  `new NM(NM())` refused, "has no constructor whose parameter types match these arguments";
  `new NM{NM(4)}` refused the same way; `new Sx(Sx(4))` copies once (copies=1, clang 0, dtors 1 at
  first show, clang 0); `new Sx(Sx(Sx(4)))` copies twice (copies=2, clang 0).
  Repro: `struct NM { NM() {} explicit NM(int); NM(NM&&) = delete; }; new NM(NM());` and
  `struct Sx { explicit Sx(int); Sx(const Sx&) {++copies;} }; Sx* p = new Sx(Sx(4));`.
  Cause (2026-11-10): a functional-cast `T(args)` is a constructor call that builds an alloca temp; a
  `new` block cannot adopt that temp (a declaration does), and the armed block is not consumed by the
  constructor call (no sret consumer fires for it, LLVMBackend_Overloads.cpp ~11192/11214). Fix
  direction: build the constructor call directly into a given object address for a new-argument.
  Same copy in a declaration (outside this bullet's scope, not fixed): `Sx s = Sx(Sx(4));` copies
  once on master (clang 0).
- Not reproduced 2026-11-09: `new CVA(4)` through a `using` alias (master and fix both run 8, clang 8).
