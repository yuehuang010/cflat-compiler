# C++ reference-member leftovers after T47: named aggregate init, const array-reference writes, const C& return ranking

Found by T47 reviews 1-3 (2026-10-09). All are pre-existing on master 608183756. Repros are in scratch/repro_keep/t47/.

1. Named (designated) aggregate initialization of a C++ record with reference members is refused. The positional braces form works. The clang designated equivalent passes. Repro: rev_t47_named.cb, with rev_t47_oracle.cpp as the oracle.
2. A write through a `const int(&)[3]` member (`h.a[0] = 5`) is accepted; clang refuses it. Repro: rev_t47_r3_constarraywrite.cb/.cpp.
3. Overload choice between `f(C&)` and `f(const C&)` for a `const C&` call result picks `C&` (exit 22); clang picks `const C&`. Repro: rev_t47_r2_constcopy.cb, with rev_t47_r2_axes.cpp as the oracle.

Fix direction: item 1 is in the designated-initializer path for C++ records. Items 2 and 3 should carry `const` provenance from the referent type: the array element for item 2, and the call result into overload ranking for item 3.
