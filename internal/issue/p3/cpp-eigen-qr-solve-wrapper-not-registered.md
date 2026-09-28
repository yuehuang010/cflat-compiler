# C++ member function template `solve` on an Eigen decomposition: "generated wrapper could not be registered"

```
no instantiation of C++ function template 'Eigen.ColPivHouseholderQR$Eigen.Matrix$double$.2$.2$.0$.2$.2$int.solve'
accepts these argument types (Eigen.ColPivHouseholderQR$..., Eigen.Vector2d)
(clang: the generated wrapper could not be registered)
```

from `Eigen.Vector2d x = Eigen.Vector2d(a.colPivHouseholderQr().solve(b));` (Eigen 5, brew). Note the
mangled `$...$` name leaks into a user-facing diagnostic (invertible-mangling ruling: every
user-facing surface demangles). Found 2026-09-26 migrating the Eigen probes to `test_libs/`.

## Repro

Library-bound for now: `test_libs/eigen/eigen_07_qr_solve.cb`
(`./test_libs.sh -t 2 --include-disabled eigen` -> must report XPASS after the fix).
`solve` is `template <typename Rhs> const Solve<Derived, Rhs> solve(const MatrixBase<Rhs>&) const`
on `SolverBase<Derived>` (CRTP base) and returns an expression-template specialization - it may
overlap `cpp-inherited-static-members-and-member-operators-not-found.md` (base-class lookup) and
`cpp-operator-returning-unrequested-specialization-not-bound.md` (unrequested return type). Reduce
first: check which of the three shapes (inherited, template, specialization return) triggers it.

## Fix direction

Not investigated. Start from where "the generated wrapper could not be registered" is raised and
log the wrapper source it tried to register.
