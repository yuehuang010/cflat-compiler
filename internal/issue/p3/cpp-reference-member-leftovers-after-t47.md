# C++ reference-member leftovers after T47: named aggregate init, const array-reference writes, const C& return ranking

Found by T47 reviews 1-3 (2026-10-09). All are pre-existing on master 68bfae05f. Repros are in scratch/repro_keep/t47/.

1. Named (designated) aggregate initialization of a C++ record with reference members is refused. The positional braces form works. The clang designated equivalent passes. Repro: rev_t47_named.cb, with rev_t47_oracle.cpp as the oracle.
2. A write through a `const int(&)[3]` member (`h.a[0] = 5`) is accepted; clang refuses it. Repro: rev_t47_r3_constarraywrite.cb/.cpp.
3. Overload choice between `f(C&)` and `f(const C&)` for a `const C&` call result picks `C&` (exit 22); clang picks `const C&`. Repro: rev_t47_r2_constcopy.cb, with rev_t47_r2_axes.cpp as the oracle.

Fix direction: item 1 is in the designated-initializer path for C++ records. Items 2 and 3 should carry `const` provenance from the referent type: the array element for item 2, and the call result into overload ranking for item 3.

## T104 run (2026-10-10)

Item 2 passes in this worktree. The new const-array write error leg rejects the store on both cold and warm
header caches; the master executable accepts it. Item 3 (C3, const C& call result overload) is OPEN and its code was reverted. A candidate-loop guard (class reference
parameter vs const-referent argument, LLVMBackend_Overloads.cpp) returned 2 for the call result, matching clang, but its
parameter-side `!Pointer` term excludes class reference params (they carry Pointer=1); dropping that term makes the ctor wrapper
`__cflat_ctor_*` for std::error_code(int, const error_category&) reject its param (wrapper spelling loses const:
`std::error_category &`), regressing std/std_11_errors_regex. Fix the ctor wrapper param spelling first, then re-apply
the guard. Probe: scratch/m104a.cb. Item 1 (designated aggregate initialization) was not attempted and remains active.
