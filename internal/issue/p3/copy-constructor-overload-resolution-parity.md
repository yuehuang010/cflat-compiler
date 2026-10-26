# Copy constructor overload resolution differs from clang

Summary: Const copy assignment chooses a different overload and return initialization adds a copy.
Repro: `y = x` with const `x` and `operator=(CV&)` / `operator=(const CV&)`; `rv.CV y = f(rv.CV(7))`.
Master vs expected: master returns 17 vs clang 27 and makes one extra copy (107 vs 7).
Found: T461 r1 opus review, 2026-10-24 box, PRE-EXISTING
