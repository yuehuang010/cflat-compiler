# Typedef of imported C++ type is unresolved

Summary: A typedef name for an imported C++ type is not registered for later use.
Repro: `typedef t261.Counted CC;` then use `CC`.
Master vs expected: master reports `cannot find the type CC`; expected the typedef name to resolve.
Found: T440 r1 opus review, 2026-10-24 box, PRE-EXISTING
