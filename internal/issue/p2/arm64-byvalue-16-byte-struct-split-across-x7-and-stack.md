# arm64: a 16-byte C struct passed by value after 7 int args is split between x7 and the stack

Found by the T174 Opus review 1 (2026-10-14), pre-existing on master. Calling a C function `f(int a1..a7, struct {char c; double d;} s)`
(and an `{int; int; double}` mid-list variant) from CFlat: CFlat lowers the 16-byte struct as two i64 params, so the first half
lands in x7 and the second on the stack. AAPCS64 (and clang) puts the WHOLE composite on the stack once it does not fit in the
remaining GPRs (NGRN + 2 > 8). Wrong values: CFlat 7 / 7707, clang 507 / 607.
Repro: scratch/repro_keep/t174_rev1/abi.c (+ the .cb callers many_cd / many_mid in the T174 review probe).
Fix direction: C-ABI argument lowering (the clang CodeGen ABIArgInfo path CFlat uses for C calls) must not split a composite
across registers and stack on arm64 - use clang's arrangeFreeFunctionCall coercion (it emits [2 x i64] which LLVM keeps whole) or
mark the remaining GPRs consumed. Check the reverse direction (C calling a CFlat function pointer with that signature) too.
