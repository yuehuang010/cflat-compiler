# Ternary move-join leftovers (lowered-struct, [cpp] struct and C++ class shapes)

Re-measured 2026-11-19 (TERNLEFT) against clang++ -std=c++20. Types: `cppbv.CntA` / `cppbv.TernaryCnt`
(Test/library/cpp_t231_byvalue.hpp, counted ctors/copies/moves/dtors), `struct H { cppbv.CntA c = cppbv.CntA(0); }`
(a lowered CFlat struct), `[cpp] struct LU { int x = 0; cppbv.CntA item; LU(int v) { x = v; } ~LU() { lu_d++; } }`.
Closed by TERNLEFT (legs 99600-99625, 8306-8313): lowered `z = k ? move a : move b` (was +1 move/dtor, the
flat_host shape), by-value parameters moved by a `?:` arm (were never destroyed), discarded mixed joins of a
lowered host or an imported class (no temp; `h1` kept 21 instead of clang's -1), lowered [cpp] `auto r = k ? LU(4) :
LU(5)` (+1 move/dtor) and `return k ? LU(4) : LU(5)` (refused). `b4_r_c1_rev` (`C h(bool k) { const C a; C b;
C c = k ? move b : move a; return c; }`, `z = h(k)`) now matches clang (z=22, 1 copy, 0 moves, 1 move-assign).

## Open
Re-measured 2026-11-19 (TERNREST, probes scratch/ternrest_tmp/ in the ternrest worktree). Closed by TERNREST
(legs 99653-99704): nested discarded joins of an imported class (`k ? move h1 : (k ? h2 : move h3);`, any depth),
nested all-move lowered assignments and discards (`z = k ? move h1 : (k ? move h2 : move h3)`, no inner temp),
discarded unmoved place arms that are no named variable (`*pc`, `p->f`, `a[i]`, implicit-this field; lowered and
imported), [cpp] struct `auto r = k ? move a : LU(5)` (was refused) and the discarded `k ? move a : LU(5);` (was a
byte move). Oracle for the [cpp] struct cells: the class as CFlat declares it to clang (deleted copy, member-wise
move ctor, user dtor; [cpp] struct ruling: no implicit copy), not a plain C++ struct with an implicit copy.
- Argument order (counts only, no leak): `int Take2(HT a, HT b)`, `struct HT { cppbv.CntA c; cppbv.TCopy t; }`,
  `h2.t.v = -2000` (its copy throws), `Take2(h2, k ? MkHT() : move h1)` under a C++ catch. CFlat evaluates the join
  (k=1: builds `MkHT()`, CntA c=3 d=4; k=0: moves `h1`, mv=1) before copying `h2`; clang copies `h2` first, throws,
  never evaluates arg 2 (c=2 d=3, mv=0). Root cause: by-value argument materialization runs after later arguments.
- Source reads after a join stay as master: imported classes allow `a.v` after `z = k ? move a : b` (frozen T263/T288
  legs read it, clang allows it); a lowered host refuses it like a direct move.
- Const by-value parameter (`void f(const rv.C a)`): CFlat keeps no const on by-value params, so `z = k ? move a :
  move b` moves `a`; clang copies. Needs const carried on parameter signatures.
- `_ = move w.lu` on a [cpp] struct FIELD (as for a [cpp] local) destroys in place, no move ctor (item.v 0); clang moves
  into a temp. movedCxxPlace excludes IsCppStructName on purpose (CFlat release design). Same on master.
- Discarded `k ? x : cppbv.ternaryGetRvalue()` with a `T&&` call result, `k=false`: clang moves from the
  referent (value -1, ct/cp/mv/dt=1/0/1/2); master and TERNLEFT3 round 2 leave it intact (3, 1/0/0/1).
  The `k=true` copy case is fixed by round 2 (leg 99810). The sameDirectClassType category now admits the
  reference join, but the selected rvalue-reference call is not materialized as a move in the discard path.
  Fix direction: move the selected T&& referent into the discarded join temporary while preserving the
  lvalue arm's copy.

- TERNLEFT3 review 2 (2026-11-22, same on master): a join used as a CONDITION operand with its own direct const leaf,
  `(k ? *pc : move h1).v > 0 ? ... : ...` at k=0, moves h1; clang copies it.
- Same review: a discarded NESTED join of places next to a prvalue, `k ? mk(9) : (j ? *getp(&x) : px[idx()]);`,
  makes no copy of the selected place; clang copies it (sameDirectClassType excludes nested arms on purpose).
- Same review: CFlat refuses two joins clang accepts: `k ? px : gpref()` (`T*` against a `T*&` call) and
  `k ? grv() : mk(9)` (a `T&&` call against a prvalue, "incompatible types 'pointer' and 'rv.T'").

## Out of scope here (unchanged, not re-measured)
- By-value first-argument unwind leak (`take2T` with the FIRST argument's copy throwing), master the same.
- `rq.cref(k ? mkC(1) : mkC(2))` one extra move+destroy vs clang; the direct `rq.cref(mkC(1))` has the same
  mismatch (CFlat-return materialization, not ternary-only).
