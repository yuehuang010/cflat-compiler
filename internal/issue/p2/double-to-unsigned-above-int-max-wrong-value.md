# Explicit (unsigned) cast of a double above INT_MAX gives the wrong value

Found in T78 round 2b (pre-existing on master). Repros: scratch/repro_keep/t78/ctr2.cb,
rev_t78_r3_unsigned.cb.

`double big = 3000000000.5; (unsigned)big` is not 3000000000 in CFlat; clang returns 3000000000.
Likely lowered as fptosi to i32 (poison/garbage above INT_MAX) instead of fptoui. Workaround used
in Test/test_cpp_interop.cb ~6277: `(unsigned)(i64)x`.

Fix direction: explicit and implicit float -> unsigned conversions emit fptoui (check every cast
door, incl. constant folding).
