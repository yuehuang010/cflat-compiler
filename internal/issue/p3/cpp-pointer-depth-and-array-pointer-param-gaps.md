# C++ pointer-depth mismatch accepted

Found by the T48 round-5 Sol review (2026-10-05). PRE-EXISTING on master.

Root cause (ptrdepth r1, 2026-11-08): MapCTypeToTypeAndValueImpl collapses pointer depth >= 3 to void**, so int*** is indistinguishable from void**; a depth guard on top of that broke depth-1-to-2 pointer arguments (26 test_libs tier-2 failures).

## Repros (scratch/repro_keep/t48_r5/)

- d37: a non-null `int**` passed to a lone C++ `int***` parameter is accepted (prints 99);
  clang refuses. Native CFlat `int**` into `int*` refuses like clang.

## Fix direction

Preserve pointer depth >= 3 in MapCTypeToTypeAndValueImpl before enforcing C++ pointer-depth conversions.
