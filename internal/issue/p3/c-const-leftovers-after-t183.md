# C const leftovers after T183

Filed 2026-10-14 from the T183 Opus review (PRE-EXISTING). Repros scratch/repro_keep/t183_rv1/.
1. CFlat-source prototypes carry no pointee const, so `memset(r_karr, 0, 12)` / `strcpy(r_str, "a")` (runtime-declared) and a
   user `cflatTake(int*)` accept a const C global and SIGBUS (138). The C++ path accepts the same. Exempt by design in T183
   (CFlat prototypes have no const); a fix needs the runtime libc prototypes to come from the real headers or carry const.
2. `const int* arr[N]` (array of pointer-to-const): `*r_parr[0] = 1` compiles and SIGBUS (clang errors); `r_take(r_parr[0])`
   accepted (clang warns). Element pointee const not carried for arrays of pointers.
3. `r_rs.a = 1` writes a const FIELD of a non-const C struct (clang errors); accepted, no crash.
(`int* p = r_karr; *p = 9;` - pointer init drops const - is T184's scope.)
