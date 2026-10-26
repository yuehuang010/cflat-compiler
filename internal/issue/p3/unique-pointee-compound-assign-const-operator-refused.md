# Compound assignment through mutable pointer to const pointee is refused

Summary: Compound assignment through a mutable pointer field to a const pointee is rejected although C++ selects a const operator overload.
Repro: `*pp.mcp += 1`.
Master vs expected: master refuses; clang calls the const `operator+=` and returns 82.
Found: T450 r1 opus review, 2026-10-24 box, PRE-EXISTING
