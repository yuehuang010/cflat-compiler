# C++-class fields of CFlat structs: default-construction and initializer leftovers after V20

V20 (2026-10-01) fixed two things:
- a field initializer in a no-arg user ctor is now built in place (no temp, no extra dtor), only for a
  single-call initializer, so a ternary takes the selected arm;
- the no-default-ctor refusal now recurses into nested structs for `W w;`.
All items below also happen on master. Probes: scratch/repro_keep/v20/rv20/ (c = initializer cells with
o.cpp clang++ oracle, d = nested refusal cells; cnt.h counts def/arg/copy/move/assign/dtor).

1. (p2) Several nested default constructions still compile silently and leave a deleted-default
   C++ field (cnt.Del) zeroed:
   - `W w = W()`, `new W()`, `W[2] a;`, `X x = X()`, `G<H> g = G<H>()`;
   - a global `W gw;`;
   - user ctors `W(){}` / `W(int)` that never initialize `h`;
   - `H[2]` fields.
   Here `W { H h; }` and `H { cnt.Del d; }`. One level deep (`H h = H()`) is refused.
   Fix site: the zero-arg ctor-call check in LLVMBackend_Overloads.cpp (~3025) skips nested CFlat
   struct fields; recurse the same way MainListener_Aggregates.cpp's V20 nested check does.
2. (p2) A temporary passed as an argument inside a field initializer is never destroyed (dtor 1 vs
   clang++ 2), e.g. `cnt.C c = cnt2.twice(cnt.C(4));`. Cells u9/u10/u13/n9.
3. (p3) A brace field default on a struct without a user ctor loses the C++ field's value:
   `struct W { H h = { d = cnt.Del(5) }; };` then `W w;` gives d.v = 0 (d/p.cb). With a user ctor it
   is correct.
4. (p3) A field initializer that names an earlier field is "Undefined variable" (C++ allows it).
   Rule needed before fixing.
