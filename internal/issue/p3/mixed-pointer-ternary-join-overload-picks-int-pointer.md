# Mixed P* ternary join picks read(int*) over read(P*)

PRE-EXISTING (T107 review 1; master and T107, normal and --asan). With overloads `read(int*)` and
`read(P*)`, a call whose argument is a ternary joining a `P*` arm with a mixed arm (see repro)
selects `read(int*)` (tag 1); clang++ -std=c++20 selects `read(P*)` (tag 2).
Repro: scratch/repro_keep/t107/rev_t107_overload.cb.
Fix direction: the joined type of the ternary must be the common pointer type (P*) before overload
ranking; find where the join's result type is taken from the field-address arm.
