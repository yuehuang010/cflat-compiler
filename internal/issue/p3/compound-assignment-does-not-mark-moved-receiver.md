# Compound assignment does not mark moved receiver

Summary: A member `&&` compound assignment accepts a later read of its moved receiver.
Repro: `(move k) -= 1; k -= 1;`.
Master vs expected: master accepts use after move; related member call and free-operator forms diagnose it.
Found: T455 r1 opus review, 2026-10-24 box, PRE-EXISTING
