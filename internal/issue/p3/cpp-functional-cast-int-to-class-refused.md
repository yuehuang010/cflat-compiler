# Functional cast from int to C++ class is refused

Summary: CFlat rejects a functional cast that invokes a C++ converting constructor.
Repro: `(R)(7)` where C++ class `R` has `R(int)`.
Master vs expected: master reports scalar-to-struct; clang returns 7.
Found: T463 r2 sol review, 2026-10-24 box, PRE-EXISTING
