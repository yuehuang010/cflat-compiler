# C++ member-template overloads reported ambiguous for int-literal arguments (Eigen `block`)

`m.block(0, 0, 2, 2)` on an `Eigen.Matrix3d` (Eigen 5, brew) fails:

```
cannot choose which 'block' to call: block(long, long, int, long), block(long, long, long, int).
Cast the argument to the type you want.
```

clang picks one overload for the same call in C++ (Eigen declares `block` as templates over the
row/column count types, `NRowsType` / `NColsType`). With every argument cast to `(long)` the call
compiles and runs correctly. Found 2026-09-26 while migrating the Eigen probes to `test_libs/`.

## Repro

Library-bound for now: `test_libs/eigen/eigen_06_block_int_args.cb`
(`./test_libs.sh -t 2 --include-disabled eigen` -> must report XPASS after the fix).
Not yet reduced to a standalone header - first step for the fixer: a class with two member
templates `template <class R, class C> X f(long, long, R, C)` overload shapes mirroring Eigen's
`block` declarations (read them in `Eigen/src/plugins/BlockMethods.inc`), called with int literals.

## Fix direction

Not investigated. Likely the CFlat side instantiates candidates with deduced types that differ per
argument and then ranks them itself instead of letting clang's overload resolution choose; compare
with the clang-parity tie-breakers landed in d8d21f18.
