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
T226 review 1 (pre-existing on master): `static T g = move a` fixed by T233; `B mk(){ D d = D(6); return d; }` copies where C++20 implicit-moves.

## Left after T243 / TERNMOVE (Opus review 1; repros scratch/repro_keep/t243_rev/)
- Array element of a class temp remains an lvalue arm: `c ? rv.mkHA(1).a[0] : rv.D(2)` gives `1 2 0 1 3`
  (clang `1 2 0 2 4`). Reversed arm order has the same mismatch when the element arm is selected. The
  nested-field case `c ? rv.mkH2(1).h.d : rv.D(2)` is fixed by TERNMOVE; the array GEP path still needs
  to be traced back to the owning temporary alloca.
