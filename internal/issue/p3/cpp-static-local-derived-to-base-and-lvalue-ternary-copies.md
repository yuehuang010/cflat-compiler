# Static locals of C++ class type: leftovers after T93, T226, T233 and T243

Found by T93 review r1 (2026-10-10). Probes: scratch/repro_keep/t93/ (rev_t93_r1.sh drives them; compare
cflat vs clang). T226 fixed the lvalue copy and the non-ternary derived slice. T233 fixed the static
`move` alias, the derived/mixed ternary arms into a base local or static (clang counts, legs 93015-93024).
T233 round 2 fixed the xvalue (`move d`) derived arms, mixed lvalue/xvalue/prvalue arms of one derived
class, `static T g = move *p`, and the moved-source refusal for a second static `move a` (legs
93025-93035). T231 fixed the call-argument double slice `takeb(D(3))`. TERNBASE (af28ab81) fixed the
derived/base lvalue join. TERNMOVE fixed the nested-field arm `rv.mkH2(1).h.d`. STATELEM fixed the
array element of a class temporary as a ternary arm in typed and `auto` locals (legs 93085-93100).
STATCONST fixed a const field of a temporary (or a move of one) as a derived arm joining a base local: the join copies as clang (legs 93109-93116).

Still open:

1. Global C++ class initialized from `move` of another global: cflat refuses any non-constant global
   C++ class initializer ("global variable initializer must be a compile-time constant"), so the
   global move count cannot be compared with clang yet. Not a move bug; revisit with global init design.

## Return of a derived local or parameter (fixed, STATLEFT; legs 93055-93060)

`B mk() { D d = D(6); return d; }` moved nothing: `return d;` of a bare derived local or parameter went
through a copy-only variadic-ctor wrapper (its `sl::D&` parameter copies). C++20 treats it as an xvalue,
so clang moves. The fix requests that wrapper with the returned name as an rvalue first (clang's
copy-init overload resolution on the xvalue) and falls back to the lvalue wrapper (legs 93061-93063:
`B(const D&)` only, `B(const D&)` + `B(D&&)`, explicit `B(B&&)`). Matrix in scratch/statleft_matrix.md. Parenthesized returns and
reference returns already matched clang and are guarded by legs.

## Not fixed here: refusals and notes (STATELEM scope left out)

Refused on master and still refused, clang accepts (not counts; out of this item, do not fix here):
`takeB(c ? mkHA(1).a[0] : sl.D(2))` (by-value arm ownership error; master words it "'?:' arms differ in
ownership", the STATELEM build words it "'?:' arms handle ownership differently").

Accepted since STATELEM, matching clang's counts, not legged: `sl.D g = c ? sl.D(2) : mkHA(1).a[0]` (refused
on master; element-selected `1 2 0 1 3`) and `sl.D g = c ? mkH(1).d : sl.D(2)` (refused on master;
`1 1 0 1 2` / `2 1 0 0 1`).

