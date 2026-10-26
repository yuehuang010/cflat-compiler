# Conditional skips copy of selected C++ lvalue

Summary: A conditional prvalue omits the copy when its selected arm is a C++ lvalue.
Repro: `c ? (Box)b : o` with `c` false.
Master vs expected: CFlat counters are `7/2/0/0/2/0`; clang counters are `7/3/1/0/3/0`.
Found: T454 r1 opus review, 2026-10-24 box, PRE-EXISTING
