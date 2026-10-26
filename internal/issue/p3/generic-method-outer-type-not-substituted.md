# Generic method does not substitute outer type in tuple parameter

Summary: A generic method's tuple parameter fails to substitute the enclosing generic type.
Repro: `W<T>::m<U>` with parameter `(T,U)`.
Master vs expected: master cannot resolve `T` in the method tuple parameter; expected enclosing type substitution. Probe: `scratch/repro_keep/box1022/t433/rev_t433_opus_probe.sh`.
Found: T433 r2 opus review, 2026-10-24 box, PRE-EXISTING
