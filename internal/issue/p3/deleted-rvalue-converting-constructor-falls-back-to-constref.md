# Deleted rvalue converting constructor falls back to const reference

Summary: CFlat selects a const-reference constructor when the rvalue converting constructor is deleted.
Repro: Return a value where conversion has a deleted `&&` converting constructor and a viable `const&` constructor; check both `return s;` and `return move s;`.
Master vs expected: master silently uses the const& constructor; clang rejects both forms as ill-formed.
Found: T446 r1 opus review, 2026-10-24 box, PRE-EXISTING
