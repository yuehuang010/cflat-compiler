# C const leftovers after T183

Filed 2026-10-14 from the T183 Opus review (PRE-EXISTING). Repros scratch/repro_keep/t183_rv1/.
1. CFlat-source prototypes carry no pointee const, so `memset(r_karr, 0, 12)` / `strcpy(r_str, "a")` (runtime-declared) and a
   user `cflatTake(int*)` accept a const C global and SIGBUS (138). The C++ path accepts the same. Exempt by design in T183
   (CFlat prototypes have no const); a fix needs the runtime libc prototypes to come from the real headers or carry const.
2. `const int* arr[N]` (array of pointer-to-const): `*r_parr[0] = 1` compiles and SIGBUS (clang errors); `r_take(r_parr[0])`
   accepted (clang warns). Element pointee const not carried for arrays of pointers.
3. `r_rs.a = 1` writes a const FIELD of a non-const C struct (clang errors); accepted, no crash.
(`int* p = r_karr; *p = 9;` - pointer init drops const - is T184's scope.)

Re-checked 2026-10-18 after const iteration 1 (master ecf28e477): items 1-3 still compile (c_r_memset, c_r_strcpy_dst,
c_r_cflat_fn, c_r_parr_w, c_r_parr_pass, c_r_rs_a all rc 0). Iteration 1 covered CFlat-declared extern prototypes, C
params and C struct fields, but not C header GLOBALS (`extern const int r_karr[3]`, `extern const int* r_parr[2]`) or a
const field of a non-const C struct global. Fix direction: plan item 10 - CGlobalEntry::isConst (and element pointee const
of global arrays) become ConstMask on the global's TypeAndValue, so phases 1-3 enforce them unchanged.
