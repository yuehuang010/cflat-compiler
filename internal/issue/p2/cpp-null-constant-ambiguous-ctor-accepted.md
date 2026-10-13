# C++ ctor call with a literal 0 is accepted where clang calls it ambiguous (single pointer/integral pair)

From T87 (2026-10-10), pre-existing on master. `struct S7 { S7(long); S7(void*); }` with `new S7(0)`:
clang refuses (both candidates are a conversion from 0), CFlat silently takes `S7(long)` (v=11).
`S8(long, double)` + `S8(int*, double)` with `(0, 1.5)` is the same shape (clang ambiguous, CFlat v=11).
Repro: scratch/t87m/cell_c11.cb, cell_c12.cb (+ t87_null.h) in the T87 worktree; oracle cell_c11.cpp.

Cause: the listed-constructor path (SelectCxxConstructorListed, LLVMBackend_CInterop.cpp) treats a
null pointer constant as not viable for a pointer parameter, so the pointer candidate never reaches
ranking and no ambiguity is seen. A wrapper route (T87 fix) makes clang decide; S6 (`short` / `int*`)
is refused that way, but these two are not routed to clang.

Fix direction: route a call with a null pointer constant beside a pointer-parameter constructor through
clang resolution (as CxxCtorForwardsScalarReference does for scalar references), or rank the null
pointer conversion in the listed path so ties become ambiguity. Adds a refusal, so it needs its own
accept-set and an expect_error leg in Test/errors/err_cpp_suffixed_literal_ambiguous.cb.
