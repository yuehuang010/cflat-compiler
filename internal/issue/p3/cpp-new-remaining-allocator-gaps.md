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
  RULED 2026-11-18 (maintainer): KEEP ACCEPTING, by design. CFlat has no volatile, so a copy from a `volatile DM&`
  source is an ordinary copy; the divergence from clang is recorded here and no volatile mask is planned. This
  bullet and the dm_vol bullet above are closed (no legs flip); pin the accept as a value leg when this file is next touched.
- (fixed NESTCAST r2) Same-class nested functional casts in a DECLARATION: the call-between, method-result
  and function-name collision shapes now match clang (legs 4025, 4030-4037). Still open, by design or out of scope:
  - File-scope `Sx g = Sx(Sx(4));` refused "not a compile-time constant" (by design).
  - Assignment `s = Sx(Sx(4));` copies 1, clang 0: assignment is not elision, unchanged.
  - A same-class call result routed through a generated `__cflat_free_` wrapper (a C++ `f` whose overload set needs the
    wrapper, e.g. `cppne.f` after the t460 header import in Test/test_cpp_interop.cb): `Sx s = Sx(f(Sx(4)))` copies 2,
    clang 1; the sret slot does not survive the wrapper (NESTCAST residue, no worse than master).
  - Bare `Sx(...)` inside a reopened namespace that also sees a global function `Sx` is refused; clang accepts (pre-existing).
- Not reproduced 2026-11-09: `new CVA(4)` through a `using` alias (master and fix both run 8, clang 8).
