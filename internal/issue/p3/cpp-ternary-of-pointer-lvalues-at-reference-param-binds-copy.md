# Ternary of pointer lvalues at a C++ reference parameter binds a temporary

T67 review 2 (2026-10-07); master same. `addr_cref(t ? p : o)` (C++ `int* const&`) binds a copy where
clang binds `p` itself; `bump(t ? p : o)` (`int*&`) is refused "cannot bind to a temporary" where clang
bumps `p`. Bridge ruling 2026-09-21 (ternary collapses to the selected arm; ternary lvalue kept) says
both-lvalue ternaries are lvalues. Probes: scratch/repro_keep/t67_rev/r2/.
Fix direction: when both arms are lvalues of the same pointer type, pass the selected arm's slot
(select of addresses) to reference params, as the class-type ternary path does.
