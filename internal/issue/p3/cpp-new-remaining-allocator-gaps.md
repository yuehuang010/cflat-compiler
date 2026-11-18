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
- Same-class nested functional casts in a DECLARATION: the sole-argument path (TryDeclareForeignCxxLocal, MainListener_Declarations.cpp ~9336) now builds the inner cast into the declared slot, as the `new` path does (legs 4021-4024). Still open, all probed with clang 0 / master-equal:
  - Call between, `Sx s = Sx(f(Sx(4)))` with `Sx f(Sx x) { return x; }`: copies 2, clang 1.
  - Method returning the same class, `Sx s = Sx(Sx(4).m())`: copies 1, clang 0.
  - Alias spelled inside an argument: `using SA = Sx; SA s = SA(SA(4));` refused "Undefined variable SA" (single-level `SA s = SA(4)` works; `new SA(SA(4))` refused the same way).
  - File-scope `Sx g = Sx(Sx(4));` refused "not a compile-time constant" (by design).
  - Assignment `s = Sx(Sx(4));` copies 1, clang 0: assignment is not elision, unchanged.
- Not reproduced 2026-11-09: `new CVA(4)` through a `using` alias (master and fix both run 8, clang 8).
