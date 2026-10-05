# Ambiguous friend operator call silently picks the registered overload

`X` with friend `==` overloads for `(X, X)`, `(X, double)`, `(X, int)`; `x == 1L` is AMBIGUOUS in clang
(long -> int and long -> double are both conversions of the same rank), but CFlat picks `(X, X)` (value 10).
Same on master 33866560 and after T12; since T12 sends mixed-type comparisons on classes with friend operators to
clang (ADL) first, clang's ambiguity diagnostic is swallowed and the registered overload wins.
Probes: scratch/repro_keep/t12/ (p3.hpp).

Fix direction: when the ADL request fails as ambiguous, relay the clang diagnostic ("clang: " prefix ruling)
instead of falling back to the registered candidates.

Found by: T12 review, fix timebox 2026-10-02.
