# Template member operators are missing from candidate ranking

Summary: Template member operators are not ranked alongside free operators or included when they are the only candidate.
Repro: Protected `template<class T> operator+(T)` plus free `operator+(const PT&, double)`, then `t + 2`; template-only member and `*this + 2`.
Master vs expected: master picks the free candidate or reports no operator; clang selects the protected member and rejects access, or ranks the member candidate.
Found: T456 r2 opus review, 2026-10-24 box, PRE-EXISTING
