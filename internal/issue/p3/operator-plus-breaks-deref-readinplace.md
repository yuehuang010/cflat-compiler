# Addition breaks dereference read-in-place tracking

Summary: Adding an operator step to a dereference operand chain prevents read-in-place handling and leaks call temporaries.
Repro: `s = s + *(mk()->heap + 1);` and `int k = *(mk()->heap + 1) + 0;`; retained repro `scratch/repro_keep/box1022/t429/rev_t429_opus_readinplace.cb`.
Master vs expected: master destroys zero of ten call temporaries; expected each read-only temporary to be released after the statement.
Root cause: `+` breaks `IsDerefOperandChain`, so `readInPlace` does not fire.
Found: T429 r1 opus review, 2026-10-24 box, PRE-EXISTING
