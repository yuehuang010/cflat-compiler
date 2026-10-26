# Throw during old destination destruction leaks fresh movable temporary

Summary: An interop throw during destruction of an assignment destination leaves the fresh movable temporary undestroyed.
Repro: `mov_unwind.cb` from the T442 Sol review repro directory (worktree removed).
Master vs expected: master leaks the fresh temporary; expected cleanup during unwind.
Found: T442 r3 sol review, 2026-10-24 box, PRE-EXISTING
