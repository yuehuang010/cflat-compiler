# Literal 0 to a C++ constructor picks a different overload than clang

Found in T78 review 3 (pre-existing on master). Repro: scratch/repro_keep/t78/rev_t78_r3_nullzero.cb
(+ rev_t78_r3_null.h, oracle rev_t78_r3_nullzero.cpp).

`NullAlt(short,int)` + `NullAlt(int*,double)`, `new NullAlt(0, 1.5)`: clang selects the pointer
constructor (returns 99); CFlat selects `(short,int)` (returns 11). `0u` / `0UL` same; `nullptr`
picks 99. Silent wrong constructor = wrong code.

Fix direction: rank a null pointer constant into a pointer parameter like clang (null-pointer
conversion vs integral conversion + floating-integral conversion on the other argument), or take
clang's selected constructor.
