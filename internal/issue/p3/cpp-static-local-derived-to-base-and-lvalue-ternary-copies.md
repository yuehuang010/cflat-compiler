# Static locals of C++ class type: leftovers after T93, T226 and T233

Found by T93 review r1 (2026-10-10). Probes: scratch/repro_keep/t93/ (rev_t93_r1.sh drives them; compare
cflat vs clang). T226 fixed the lvalue copy and the non-ternary derived slice. T233 fixed the static
`move` alias, the derived/mixed ternary arms into a base local or static (clang counts, legs 93015-93024). T233 round 2 fixed the xvalue (`move d`) derived arms, mixed lvalue/xvalue/prvalue arms of one derived class (clang joins them as a temporary), `static T g = move *p`, and the moved-source refusal for a second static `move a` (legs 93025-93035, Test/errors/err_cpp_use_after_move.cb). T231 fixed the call-argument double slice `takeb(D(3))`.

Still open:

1. An exception from a temporary's noexcept(false) destructor inside the static's initializer does
   not propagate: cflat prints `3 3 made=1`, clang `-9 4 made=2` (rev_t93_cleanup.cb).
2. Global C++ class initialized from `move` of another global: cflat refuses any non-constant global
   C++ class initializer ("global variable initializer must be a compile-time constant"), so the
   global move count cannot be compared with clang yet. Not a move bug; revisit with global init design.
3. Mixed LLVM-level ternary of lvalues of different classes outside an initializer
   (`c ? d1 : b2` with D d1, B b2) is refused by the phi join ("ternary branches have incompatible
   types"). T233 covers the static/local initializer form only.
4. Same-type ternary with a moved arm (pre-existing, not a T233 regression): `T g = c ? a : move b;`
   with c false copies b (`2 2 1 0 3`), clang moves it (`2 2 0 1 3`). Same for `static T g = ...`
   (repro: `T a = T(1); T b = T(2); static T g = c ? a : move b;`, count with the T226 fixture).

T226 review 1 (pre-existing on master): `static T g = move a` fixed by T233; `B mk(){ D d = D(6); return d; }` copies where C++20 implicit-moves.

## Left after T233 r2 (Opus review 2; repros scratch/repro_keep/t233_rev2/, clang twin rv.hpp; values right, counts off by one)
- A deferred derived arm whose other arm is a NESTED ternary skips clang's temp (nested arms record no
  category): `B g = c ? d1 : (c ? move d2 : D(3))` fix 1 2 1 0 3, clang 1 2 1 1 4 (static and `!c` too).
  Direction: MainListener_Expressions.cpp ~9335 / ~9490, record a nested same-derived arm as a prvalue.
- A member of a class prvalue (`mkH(1).d`, xvalue in C++) is classified as an lvalue arm:
  `c ? mkH(1).d : D(2)` copies where clang moves; `c ? move d1 : mkH(2).d` adds a temp. Direction: ~8941,
  an arm stored in a field of an owned temp is an xvalue.
