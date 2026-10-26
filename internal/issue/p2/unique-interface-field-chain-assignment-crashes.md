# Chained unique interface field assignment crashes at runtime

Summary: Chaining assignments to unique interface fields crashes during ordinary execution.
Repro: `w.i = w2.i = new BI();` with `i` a unique interface field; T476 probe path from the removed worktree.
Master vs expected: master exits 139 for every spelling, including a plain statement; expected a valid assignment chain.
Found: T476 Opus r1 review, 2026-10-24 box, PRE-EXISTING
