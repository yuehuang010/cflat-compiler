# Const-reference and by-value overload ambiguity is accepted on the function-call path

Summary: a free function call `fw(v)` with `fw(const WV&)` and `fw(WV)` overloads picks the const
reference overload for an lvalue `WV`, where clang diagnoses the call as ambiguous. The constructor
path is fixed in MIX11 (converting constructor tie for a by-value and const& pair).
Repro: `scratch/mix11_review/l.cb` with `scratch/mix11_review/p3.hpp` (`WV v = 1; return fw(v);`).
Master vs expected: master (and the MIX11 branch) returns 1; clang (`clang++ -std=c++20`) reports
the call as ambiguous. Expected: a refusal on the function path.
Found: T459 r1 sol review, 2026-10-24 box, PRE-EXISTING. Function-path residue, MIX11 r2 review.
