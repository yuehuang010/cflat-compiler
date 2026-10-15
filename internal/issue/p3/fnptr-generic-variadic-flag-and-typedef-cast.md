# Function-pointer leftovers after T179: generics drop the variadic flag; cast to a C fn-ptr typedef fails to parse

Filed 2026-10-14 from the T179 Opus review (PRE-EXISTING).

1. `struct Box<T> { T v; }; Box<T170Vp> bx; T170Bin r = bx.v;` and `T170Bin r = id<T170Vp>(a);` compile (rc 0); clang refuses.
   The variadic flag does not survive generic substitution of a C fn-ptr typedef (missed refusal, not a false one).
2. `(T170Bin)a` -> "cannot find the type 'T170Bin'": a C function-pointer typedef is not accepted as a cast target type.
Not a finding: a mixed ternary `T170Bin t = c ? va : pa;` is accepted; clang only warns.
Repros scratch/repro_keep/t179_rv1/.
