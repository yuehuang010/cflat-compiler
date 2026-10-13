# C++ variadic calls: aggregate in `...` position and sret return give silent wrong values

PRE-EXISTING (T110 review 1; master on non-inline variadics). T110 made inline variadic free
functions and members callable, so the same shapes now reach the inline paths too (they failed to
link or were refused before T110) - the reach of this wrong-code bug grew.

1. A struct passed by value through `...`: `fstruct(1, p)` with `P2{3,4}` (8-byte POD) gives
   cflat 31, clang 35 - the second field reads as 0. Master: non-inline `rv2.nstruct` 31 vs 35.
   Apple arm64 passes small aggregates in variadic position as a coerced i64 on the stack.
2. An sret-returning variadic call passes the wrong vararg: `Big f(int n, ...)` (32-byte return),
   `f(9, (i64)11).b` gives cflat 1, clang 11. Master: non-inline `rv2.nbig` 901 vs 911; now also
   the inline free function `rv2.fbig` and member `s.bigret`. Non-variadic sret members are right.

Repros: scratch/repro_keep/t110/ (h.hpp + cases.txt / cases2.txt; probe shape in the T110
review). Fix direction: lower variadic C++ calls through clang's ABI arrangement (CodeGenTypes /
arrangeFreeFunctionCall with the variadic flag) for aggregate args and sret; until then refuse
aggregate-by-value arguments in `...` position and sret-returning variadic calls with a specific
error - never emit a call clang would arrange differently.
