# C++ pointer-depth mismatch accepted; member array-pointer params refused

Found by the T48 round-5 Sol review (2026-10-05). PRE-EXISTING on master.

## Repros (scratch/repro_keep/t48_r5/)

- d37: a non-null `int**` passed to a lone C++ `int***` parameter is accepted (prints 99);
  clang refuses. Native CFlat `int**` into `int*` refuses like clang.
- member_apr / static_ap: a C++ member or static member taking an array pointer
  (`int (*)[2]`, `int (* const&)[2]`) refuses a valid argument; clang accepts (69 / 70).

## Fix direction

Pointer depth must match exactly for C++ candidates (only a null pointer constant or
std::nullptr_t converts across depths). Member / static-member array-pointer params need the same
binding the free-function form already has.
