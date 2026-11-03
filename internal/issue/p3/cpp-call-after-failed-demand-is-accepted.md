# C++ member definition is accepted after earlier failed member demands

Summary: A non-template C++ struct member definition can be accepted after demand resolution of earlier members failed.
Repro: `struct pr { S a(){return c()+b();} };` (T424 repros: `scratch/repro_keep/box1022/t424/d2.cb`, `d4.cb`; header `pr.h`).
Master vs expected: master returns 0 and emits output when an earlier demand for `t.c()` / `t.b()` in the same file already failed; expected a diagnostic.
Found: T424 r2 opus review, 2026-10-24 box, PRE-EXISTING

Root cause (MIX11): not fixed. Verbose trace of the d2 repro in a fresh directory (copy of d2.cb and
pr.h): the live C++ group gives `_ZN2pr1S1cEv: -1` (c refused), then `_ZN2pr1S1aEv: 1` for a(), which
calls c(). The refused body is no longer in the live TU, so clang Sema accepts a()'s call and the
verdict is memoized "ok" (LLVMBackend_CInterop.cpp CheckCxxDemand, ~7711-7720). The verdict also
varies with cache state: the same d2.cb in another directory reported the expected error on every
run. Fix direction: a body that calls an already-refused member must take that member's refusal
(or the refused body must stay in the TU so clang re-diagnoses it).
