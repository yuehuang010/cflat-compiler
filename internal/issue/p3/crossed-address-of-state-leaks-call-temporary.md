# Address-of tracking leaks across unary dereference and call arguments

Summary: `crossedAddressOf` is not reset when a unary `*` or call argument boundary is crossed, producing incorrect temp lifetime handling.
Repro: `alias int* p = &arr[0] + *(&mk()->v) - 6;` and `+ f(&mk()->v)`; non-alias form `int* p = &arr[0] + *(&mk()->v) - 6` also leaks on master.
Master vs expected: master leaks the temporary; expected destruction at the end of its use.
Root cause: `crossedAddressOf` is not reset above unary `*` or at a call argument list (MainListener_PostfixExpression.cpp:93-98).
Found: T429 r3 opus review, 2026-10-24 box, PRE-EXISTING
