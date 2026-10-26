# Bare generic array deduction fails

Summary: Generic type deduction fails for an array argument in the bare call form.
Repro: `bare_array.cb` from the T463 Sol review repro (worktree removed).
Master vs expected: master fails deduction; expected the generic array argument to deduce.
Found: T463 r1 sol review, 2026-10-24 box, PRE-EXISTING
