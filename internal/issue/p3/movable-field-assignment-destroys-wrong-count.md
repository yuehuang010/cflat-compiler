# Movable field assignment destroys the wrong number of values

Summary: Assigning a movable field destroys too many tracked objects.
Repro: T442 r3 Sol review repro in the deleted worktree; described as movable field assignment.
Master vs expected: master counts `2,2,3`; expected `2,1,2`.
Found: T442 r3 sol review, 2026-10-24 box, PRE-EXISTING
