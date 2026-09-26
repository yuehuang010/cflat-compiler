Bucket: p3 (C++ interop; follow-ups left by fix/cpp-class-operator-new, 2026-09-23)

# C++ `new` / `delete`: remaining gaps after class-level operator pairing

Measured before and after fix/cpp-class-operator-new (Test/library/cpp_interop_opnew.h,
legs 3350-3366 of Test/test_cpp_interop.cb):

1. (fixed 2026-09-25, a31f4a72 + 843a9f8c) `(new T(x))->m()` is freed when the linked body of `m`
   provably does not retain `this`, in any function including returns and generic instantiations,
   and on the unwind path. Virtual / unlinked / escaping methods keep the bounded leak. Remaining
   dangling-address shapes live in p2/address-of-new-temp-member-dangles.md.
2. (not reproduced 2026-09-24: a CFlat struct field `unique CppClass*` assigned `new CppClass(...)` twice
   compiles with exact counts; re-probe the original shape before working on it)
3. (fixed 767ae268) runtime-count delete of a polymorphic C++ raw pointer.
4. (fixed 767ae268) private/protected class operator new/delete refused. Placement, aligned and
   destroying forms are still not exported.
5. Windows: the MS-ABI names for `operator new[]` / `delete[]` (`??_U@YAPEAX_K@Z`,
   `??_V@YAXPEAX@Z` and the aligned forms) are written but unexercised on macOS.

Fix direction per item: (1) register the temporary in the statement-temp destruction list with
the paired delete; (2) treat a `new` expression as owning at the field-store site; (3) loop the
deleting destructor over the runtime count; (4) refuse `new T` when the selected operator is
inaccessible; (5) run test_cpp_interop on Windows.
