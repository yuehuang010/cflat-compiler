# Expression template passed to a CFlat function taking a C++ class by value is refused

Found 2026-09-27 by fix/eigen-expr. `double take(Vec3 v)` (CFlat function, `Vec3` a C++ class with a
templated `const VecBase<D>&` converting ctor) called as `take(a + b)` -> "no overload matches"; clang
accepts the C++ equivalent (copy-initialization of the parameter through the converting ctor).
Init (`Vec3 v = a + b;`), explicit `Vec3(a + b)` and assignment already work (fix/eigen-expr).
Fix direction: the call-argument classifier (ClassifyCxxImplicitArgument path for CFlat callees) would
need a clang copy-init request during overload ranking. Compile-time cost question (p1 parity work):
only request when no other candidate matches, and cache the answer per (source, target) pair.
Fixture: namespace cppi_expr in Test/library/cpp_interop_basic.h.
