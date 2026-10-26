# Generic function parameter in tuple return type is unresolved

Summary: A generic parameter used in a function return tuple is not available during signature parsing.
Repro: generic function `mk<T>` returning `(int,T)`; `scratch/repro_keep/box1022/t433/c5b_genret.cb`.
Master vs expected: master reports `cannot find the type 'T'`; expected generic return type resolution.
Found: T433 r1 opus review, 2026-10-24 box, PRE-EXISTING
