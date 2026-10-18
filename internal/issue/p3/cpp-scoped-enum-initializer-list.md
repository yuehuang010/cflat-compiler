# Scoped enum vs C++ std::initializer_list parameters

Found by the T278 review (round 3), pre-existing on master.

## Summary
1. A scoped C++ enum constant (`enum class SC`) brace-passed to an `std::initializer_list<int>` or
   `std::initializer_list<unsigned>` parameter compiles and binds (returns 1); clang refuses with
   "no matching function" (a scoped enum never converts implicitly to an integer).
2. `std::initializer_list<SC>` as a parameter is refused by CFlat; clang accepts `f({SC::a, SC::b})` (returns 8 in
   the probe).

## Repro
T278 worktree probe set, archived at scratch/repro_keep/wt_t278/rev_t278/ (c_sc_il_iu.cb, c_sc_list.cb).

## Fix direction
Brace-element ranking (`braceElementRank`, LLVMBackend_CInterop.cpp) should refuse a scoped-enum element against an
arithmetic element type, and accept it against the same enum element type.

## More (T286 Sol review, 2026-10-15, pre-existing on master)
3. An IMPORTED unscoped enum brace list `f({E::a, E::b})` with `f(initializer_list<int>)` + `f(initializer_list<E>)`
   overloads picks the int list (201); clang picks `initializer_list<E>` (101). Repro scratch/repro_keep/wt_t286/rev_t286_sol_tmp/importedbrace.cb.
4. `q.sc("CE.one")` binds a `const char(&)[1]` parameter (67); clang rejects the array extent (string literal of length
   7 into char[1]). Accepts invalid code. Repro .../string.cb.
