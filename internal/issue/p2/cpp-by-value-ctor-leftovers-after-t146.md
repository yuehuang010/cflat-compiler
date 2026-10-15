# C++ by-value constructor argument leftovers after T146 (prvalue elision)

Filed 2026-10-13 from Sol T146 r2/r3 PRE-EXISTING findings: each reproduces on master and on the T146 branch.
Repros + clang twins: scratch/repro_keep/t146/rev_t146_sol_r2_tmp/ and rev_t146_sol_r3_tmp/ (results*.txt hold the
master/fix/clang values).

## 2. Overloaded-constructor snapshot: 111, clang 101
A by-value argument whose class has overloaded constructors reads the source after a sibling argument changed it
(r2 tmp, `snapshot*` / `member_overload*`). The T146 order rule covers the elided path; the overloaded-ctor path still
forwards by reference.

## 3. Function-pointer objects
- A const function-pointer object loses its constness at the C++ boundary (r3 tmp).
- A named `Fn` lvalue binds a `Fn&&` constructor parameter; clang refuses (r3 tmp).

Captured-closure -> std::function ctor argument refusal is tracked in p2/cpp-capturing-closure-to-std-function.md.
