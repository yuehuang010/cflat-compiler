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
- Const leaf across a nested prvalue join: `const cppbv.TernaryCnt* pc = &h2; k ? move h1 : (k ? *pc : move h3);`
  k=1 copies `h1` (cp=1, h1=21); clang moves it (mv=1, h1=-1): the const leaf makes only the INNER join const, the
  outer converts the xvalue to the const prvalue by the move ctor. k=0 matches. Same on master. Root cause:
  JoinMoveLeafConst climbs to the outermost join and reads every leaf.
- [cpp] struct mixed return refused: `LU f(bool k) { LU a = LU(7); return k ? move a : LU(5); }` is refused ("the
  '?:' arms handle ownership differently"); with `a` a by-value parameter it is refused as a deleted copy. clang
  (class as declared) moves `a` / builds `LU(5)` in the return slot. Same on master. Root cause not investigated.
- Source reads after a join stay as master: imported classes allow `a.v` after `z = k ? move a : b` (frozen T263/T288
  legs read it, clang allows it); a lowered host refuses it like a direct move.
- Const by-value parameter (`void f(const rv.C a)`): CFlat keeps no const on by-value params, so `z = k ? move a :
  move b` moves `a`; clang copies. Needs const carried on parameter signatures.
- Discarded join with NO move leaf, an unmoved place arm and a prvalue arm: `k ? x : rv.C(9);` / `k ? *px : rv.C(9);`
  (also as a for-increment) k=1 makes no copy; clang copies into a temp (cp=1, +1 dtor). Same on master.
  armMoveLeaves returns 2|2, so discardedPrvalueJoin stays false (MainListener_Conditional.cpp ~3285). TERNREST review.
- `_ = move w.lu` on a [cpp] struct FIELD (as for a [cpp] local) destroys in place, no move ctor (item.v 0); clang moves
  into a temp. movedCxxPlace excludes IsCppStructName on purpose (CFlat release design). Same on master.

## Out of scope here (unchanged, not re-measured)
- By-value first-argument unwind leak (`take2T` with the FIRST argument's copy throwing), master the same.
- `rq.cref(k ? mkC(1) : mkC(2))` one extra move+destroy vs clang; the direct `rq.cref(mkC(1))` has the same
  mismatch (CFlat-return materialization, not ternary-only).
