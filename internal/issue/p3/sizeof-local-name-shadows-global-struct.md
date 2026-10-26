# sizeof type lookup ignores local name shadowing

Summary: `sizeof(A.B)` resolves the struct type even when a local variable `A` shadows the namespace name.
Repro: Local `A` and global struct `A.B`; also nested `S.A.B` with global `A.B`.
Master vs expected: master gives 24 instead of clang 4 in the local-shadow case; method case differs (master refuses, fixed branch 16, clang 4+4).
Found: T465 r2 opus review, 2026-10-24 box, PRE-EXISTING
