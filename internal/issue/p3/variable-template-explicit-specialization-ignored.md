# Explicit variable template specialization is ignored

Summary: A C++ variable template explicit specialization produces the primary template value.
Repro: `template<> inline constexpr int V<long> = 5` in `b.h` including `a.h`; probe `scratch/repro_keep/box1022/t436/rev_t436_opus_probe.sh` (case c3).
Master vs expected: master yields 3; clang yields 5.
Found: T436 r1 opus review, 2026-10-24 box, PRE-EXISTING
