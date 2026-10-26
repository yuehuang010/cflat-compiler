# Defaulted copy constructor with user destructor is refused by-value

Summary: A type with a defaulted copy constructor and user destructor cannot be passed by value.
Repro: `DF(const DF&) = default;` plus user destructor, then pass `DF` by value.
Master vs expected: master refuses; clang accepts.
Found: T461 r1 opus review, 2026-10-24 box, PRE-EXISTING
