# Qualified outer namespace type is unresolved

Summary: A qualified type from namespace `B` cannot resolve from outer namespace `A` in the reported tuple.
Repro: `(B.Late,int)` in outer namespace `A`; `scratch/repro_keep/box1022/t433/c4c_qual_outer.cb`.
Master vs expected: master fails to find `B.Late` even when declared first; expected qualified type lookup.
Found: T433 r1 opus review, 2026-10-24 box, PRE-EXISTING
