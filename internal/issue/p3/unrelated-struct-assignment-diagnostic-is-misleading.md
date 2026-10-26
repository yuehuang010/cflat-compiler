# Unrelated struct assignment diagnostic says fixed array decays

Summary: Assigning a value of an unrelated struct type reports a misleading fixed-array diagnostic.
Repro: `A a = mkB();` with unrelated struct types `A` and `B`.
Master vs expected: master reports `fixed array decays`; expected an incompatible struct assignment diagnostic.
Found: T442 r1 opus review, 2026-10-24 box, PRE-EXISTING
