# [cpp] equal-rank brace-list overloads: clang reports ambiguity, CFlat picks the first declared

Found by the T278 review 2 (2026-10-15). Same on master and after T278. Probe harness:
scratch/repro_keep/wt_t278/rev_t278/probe.sh (case files cases*.txt) once T278 lands.

Clang reports "call to ... is ambiguous", CFlat compiles and returns the first-declared candidate (1):
- free `il_ld(initializer_list<long>)` / `(initializer_list<double>)` with `{1, 2}`, and the reversed order
- `il_dl` / `il_di` with `{1, 2.0}`
- `il_ul` / `il_lu` (unsigned / long) with `{1, 2}`
- member (ctor/method) selectors `S.m_ld`, `m_ul`, `m_lu` with `{1, 2}`

Root cause: RequestCxxBraceFunction / the member brace selector rank candidates by container kind, then (since T278) by
the worst element conversion rank; an exact tie keeps the first declared instead of refusing.
Fix direction: on an exact tie of the best rank between two viable candidates, refuse with the existing C++ ambiguity
diagnostic (the plain-argument overload path already has one - reuse its text). Every cell vs clang++ -std=c++20.
