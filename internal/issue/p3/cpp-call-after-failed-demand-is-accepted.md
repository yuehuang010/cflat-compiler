# C++ member definition is accepted after earlier failed member demands

Summary: A non-template C++ struct member definition can be accepted after demand resolution of earlier members failed.
Repro: `struct pr { S a(){return c()+b();} };` (T424 repros: `scratch/repro_keep/box1022/t424/d2.cb`, `d4.cb`; header `pr.h`).
Master vs expected: master returns 0 and emits output when an earlier demand for `t.c()` / `t.b()` in the same file already failed; expected a diagnostic.
Found: T424 r2 opus review, 2026-10-24 box, PRE-EXISTING
