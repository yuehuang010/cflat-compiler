# Copying C++ derived object with polymorphic base is refused

Summary: A C++ struct derived from a polymorphic base is treated as having a deleted copy constructor.
Repro: Copy such a struct using `std.vector.push_back(a)`; `push_back(move a)` works.
Master vs expected: master refuses; clang accepts.
Found: T464 r1 opus review, 2026-10-24 box, PRE-EXISTING
