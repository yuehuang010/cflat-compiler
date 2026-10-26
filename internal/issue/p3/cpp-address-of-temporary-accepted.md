# Address-of C++ temporary is accepted

Summary: CFlat permits taking the address of a C++ class temporary.
Repro: `&(Box)b` and `&rv.make()`.
Master vs expected: master accepts both; clang rejects taking the address of the temporary.
Found: T454 r1 opus review, 2026-10-24 box, PRE-EXISTING
