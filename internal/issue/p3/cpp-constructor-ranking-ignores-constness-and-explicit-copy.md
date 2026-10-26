# C++ constructor ranking ignores constness and explicit copy restrictions

Summary: By-value copy can select an explicit copy constructor, and one-argument constructor calls ignore argument constness.
Repro: `explicit EC(const EC&)` plus `EC(EC&)` with const source; `S1(A&)` called with const A; overload sets `S4` and `S5` from T461 r2.
Master vs expected: master accepts cases clang refuses and returns 107 vs 207, 107 vs 57, or 207 vs 77 in the described overloads.
Found: T461 r2 opus review, 2026-10-24 box, PRE-EXISTING
