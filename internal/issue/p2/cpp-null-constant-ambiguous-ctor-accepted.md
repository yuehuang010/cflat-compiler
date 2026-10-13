# C++ literal zero does not consistently rank as a null pointer constant

From T87 (2026-10-10), pre-existing on master. `struct S7 { S7(long); S7(void*); }` with `new S7(0)`:
clang refuses (both candidates are a conversion from 0), CFlat silently takes `S7(long)` (v=11).
`S8(long, double)` + `S8(int*, double)` with `(0, 1.5)` is the same shape (clang ambiguous, CFlat v=11).
Repro: scratch/t87m/cell_c11.cb, cell_c12.cb (+ t87_null.h) in the T87 worktree; oracle cell_c11.cpp.

Cause: the listed-constructor path (SelectCxxConstructorListed, LLVMBackend_CInterop.cpp) treats a
null pointer constant as not viable for a pointer parameter, so the pointer candidate never reaches
ranking and no ambiguity is seen. A wrapper route (T87 fix) makes clang decide; S6 (`short` / `int*`)
is refused that way, but these two are not routed to clang.

## T103 round 2 (2026-10-10, Haiku): leg dropped, still open

Admitting the literal-0 pointer candidate in SelectCxxConstructorListed (null constant = compatible with
a by-value pointer parameter) fixed S7/S8 but regressed `t87.S6(0)`: CFlat's no-narrowing rule refuses
`S6(short)` before ranking, so only `S6(int*)` stays viable and wins (99, clang: ambiguous). Admission
was therefore removed; `err_cpp_null_constant_ambiguous_ctor.cb` (S7/S8) was removed with it. Still open:
a fix must make the call go to clang (or tie the pointer against a same-arity integral sibling) when a
literal 0 can take both a pointer and an integral parameter. Fix-exposed side effects to re-check: a
by-value `int*` parameter with a literal 0 must receive a real null (`K(0)` with `K(int*)` gave a
non-null temporary once admission was on; T52 `tk(0)` 5 vs 6).

Fix direction: route a call with a null pointer constant beside a pointer-parameter constructor through
clang resolution (as CxxCtorForwardsScalarReference does for scalar references), or rank the null
pointer conversion in the listed path so ties become ambiguity. Adds a refusal, so it needs its own
accept-set and an expect_error leg in Test/errors/err_cpp_suffixed_literal_ambiguous.cb.

T103 review 3 (pre-existing): when the CFlat ctor ranking cannot pick a single winner (template,
variadic, inherited, user-defined-conversion candidates, ties) the call falls back to clang's
choice without the no-implicit-narrowing check, so a selected int -> char narrowing is accepted
(rev_t103_rank_udc_bad.cb, rank_source.cb return 11 on master and T103). Repros
scratch/repro_keep/t103/.
