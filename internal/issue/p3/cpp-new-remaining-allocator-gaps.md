Bucket: p3 (C++ interop; follow-ups left by fix/cpp-class-operator-new, 2026-09-23)

# C++ `new` / `delete`: remaining gaps after class-level operator pairing

Measured before and after fix/cpp-class-operator-new (Test/library/cpp_interop_opnew.h,
legs 3350-3366 of Test/test_cpp_interop.cb):

1. A C++ `new` used as a statement temporary (`(new T(x))->get();`) is never freed.
2. `f = new CppClass(x)` into a `unique T*` FIELD is refused "from a borrowed value".
3. A pointer to a polymorphic class whose array count is only known at runtime (`delete[n] p`
   with a runtime n on a `T*`, not a view) still runs element 0's deleting destructor only.
   Views and constant counts are correct.
4. A private class-level `operator new` / `operator delete` is not bound, so lookup falls back
   to the base or global operator where C++ rejects the `new` expression. Placement, aligned
   and destroying forms are not exported either.
5. Windows: the MS-ABI names for `operator new[]` / `delete[]` (`??_U@YAPEAX_K@Z`,
   `??_V@YAXPEAX@Z` and the aligned forms) are written but unexercised on macOS.

Fix direction per item: (1) register the temporary in the statement-temp destruction list with
the paired delete; (2) treat a `new` expression as owning at the field-store site; (3) loop the
deleting destructor over the runtime count; (4) refuse `new T` when the selected operator is
inaccessible; (5) run test_cpp_interop on Windows.
