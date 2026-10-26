# Two-dimensional array field constructs each element twice

Summary: A 2-D array field of a constructor type builds each element twice when a named override is supplied.
Repro: Ctor type `E[2][2]` field with named override from T468 r1 opus review.
Master vs expected: master counts 4/7; clang counts 1/4; both are balanced but CFlat performs duplicate construction.
Found: T468 r1 opus review, 2026-10-24 box, PRE-EXISTING
