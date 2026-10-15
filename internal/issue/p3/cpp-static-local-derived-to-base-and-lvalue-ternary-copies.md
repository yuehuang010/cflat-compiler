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

## Left after T243 (Opus review 1; repros scratch/repro_keep/t243_rev/, counts only, values right)
- A NESTED field or array element of a class temp is still an lvalue arm: `c ? rv.mkH2(1).h.d : rv.D(2)` 1 1 1 1 3
  (clang 1 1 0 2 3); `c ? rv.mkHA(1).a[0] : rv.D(2)` 1 2 0 1 3 (clang 1 2 0 2 4). IsFieldOfOwnedStructTemp
  (LLVMBackend_OwnershipTemps.cpp ~4726) looks one GEP deep; walk the GEP chain back to the temp's alloca.
