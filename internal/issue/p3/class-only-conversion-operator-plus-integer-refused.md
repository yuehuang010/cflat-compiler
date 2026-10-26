# Class with only conversion-to-double cannot use built-in plus

Summary: A class with only `operator double()` is not converted for built-in addition with an integer.
Repro: `class C { operator double(); }; C c; c + 2`.
Master vs expected: master refuses; clang returns 8.
Found: T460 r2 sol review, 2026-10-24 box, PRE-EXISTING
