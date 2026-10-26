# Global operator is not ranked with namespace operator

Summary: Global and namespace-scope C++ operator candidates are not ranked in one candidate set.
Repro: Equal global and namespace candidates, then a case where the global candidate is better.
Master vs expected: equal candidates produce 1 instead of clang ambiguity; better global candidate also produces 1 instead of clang 2.
Found: T460 r1 opus review, 2026-10-24 box, PRE-EXISTING
