# Const-reference and by-value constructor ambiguity is accepted

Summary: CFlat accepts a constructor overload set that clang diagnoses as ambiguous.
Repro: T459 r1 Sol review probe in `scratch/repro_keep/box1022/t459/`.
Master vs expected: master accepts; clang refuses as ambiguous.
Found: T459 r1 sol review, 2026-10-24 box, PRE-EXISTING
