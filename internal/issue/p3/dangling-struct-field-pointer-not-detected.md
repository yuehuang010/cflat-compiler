# Returning struct with dangling field pointer is not detected

Summary: A struct containing a pointer to a local is returned without a dangling-pointer diagnostic.
Repro: `MP m = { p = &x }; return m;` where `x` is local.
Master vs expected: master accepts; expected a dangling struct-field pointer diagnostic.
Found: T442 r1 opus review, 2026-10-24 box, PRE-EXISTING
