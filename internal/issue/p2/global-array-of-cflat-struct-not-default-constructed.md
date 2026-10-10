# Global array of a CFlat struct is never default-constructed

Wrong code, silent. Found by the T72 review 1 (2026-10-07); master same.
`struct W { cppa.GTrk t; int z = 5; }; W[2] ga;` leaves ga[1].z == 0 and runs no C++ field ctor
(clang++ 75). Consequence: a global `W[2]` holding a deleted-default C++ field is also accepted
silently (one level and nested) - T72's refusal never sees it.
Cause: `needsArrayDefaultInit` in cflat/MainListener_Declarations.cpp excludes global_scope.
Repros: scratch/repro_keep/t72_rev/g5.cb, g7.cb.
Fix direction: global arrays of non-trivially-initialized structs get the same element default init
as locals (in the global initializer function), routed through the T72 availability predicate;
check field defaults, C++ ctors, destruction rule (globals: no exit-time destruction per the
2026-08 global/static ruling - confirm for arrays).
