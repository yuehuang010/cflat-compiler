# Variadic calls: C++ aggregates refused, not arranged; out-of-line variadic members refused

NARROWED by T144 (2026-10-12): sret-returning C++ variadic calls now pass their scalar tail with default promotions (was
dropped: cflat 1 vs clang 11), and a by-value aggregate in a C++ `...` slot is refused (the issue's sanctioned stopgap,
Test/errors/err_cpp_variadic_aggregate.cb). T145 applied the same refusal to C externs (Test/errors/err_c_variadic_aggregate.cb).
Remaining:

2. C++ aggregates in `...` are refused instead of lowered. Lift the refusal by arranging the call through clang's ABI
   (arrangeFreeFunctionCall with the variadic flag) - the original fix direction.
3. Out-of-line variadic C++ members (declared in the class, defined outside it) are refused on master and fix; clang
   accepts. Repro scratch/repro_keep/t144/outmember.cb + oracle.hpp.
