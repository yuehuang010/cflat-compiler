# Mixed ownership ternary argument is refused

Summary: A valid conditional mixing a returned value and a C++ lvalue cannot be passed to a function.
Repro: `take(c ? rv.make2() : b)`.
Master vs expected: master refuses due to the mixed-ownership rule; clang accepts.
Found: T454 r1 opus review, 2026-10-24 box, PRE-EXISTING
