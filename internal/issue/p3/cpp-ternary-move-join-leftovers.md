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
## Out of scope here (unchanged, not re-measured)
- By-value first-argument unwind leak (`take2T` with the FIRST argument's copy throwing), master the same.
- `rq.cref(k ? mkC(1) : mkC(2))` one extra move+destroy vs clang; the direct `rq.cref(mkC(1))` has the same
  mismatch (CFlat-return materialization, not ternary-only).

## TERNLEFT5 (2026-11-22)
Tuple-element class joins remain open. Exact repro from `scratch/briefs/rev_tupown3_cpp_tuple_join_copies.cb`:

```cflat
cppbv.TernaryCnt h1 = cppbv.TernaryCnt(21);
cppbv.TernaryCnt h2 = cppbv.TernaryCnt(22);
cppbv.TernaryCnt h3 = cppbv.TernaryCnt(23);
const cppbv.TernaryCnt* pc = &h2;
cppbv.TernaryCnt x = cppbv.TernaryCnt(24);
cppbv.TernaryCnt* px = &x;
(cppbv.TernaryCnt, int) t = (k ? move h1 : (k ? *pc : move h3), 4);
(cppbv.TernaryCnt, int) u = (k ? *px : cppbv.TernaryCnt(9), 4);
auto ta = (k ? move h1 : (k ? *pc : move h3), 4);
auto ua = (k ? *px : cppbv.TernaryCnt(9), 4);
```

Against declarations initialized directly from the same joins, the first tuple copies `h1` at `k=1`
(`0/1/0/5`, h1 stays 21) where clang/declaration move it (`0/0/1/5`, h1 becomes -1). The second tuple
adds one copy and destructor at each selector: k=0 tuple `1/1/0/6` vs declaration `1/0/0/5`; k=1
tuple `0/2/0/6` vs declaration `0/1/0/5`. A prototype construction into the parsed typed field removed
one temporary destructor but still copied that field when the tuple literal initialized its declared
storage. Fixing the full path requires threading the final tuple destination through declaration
initialization. The inferred `auto` forms above also need an element destination before tuple type
inference and remain open with this design work.

## Return of a pointer / conversion-operator class join (needs a maintainer ruling)
With `struct T { int v; }; struct PC { T* p; PC(T* q) : p(q) {} operator T*() const { return p; } };`,
`T* f(bool k, T* px, T* y) { return k ? px : PC(y); }` is refused ("cannot return the address of a '?:' arm" /
points into this function's own frame); clang accepts (PC returns the parameter). It cannot be told apart from
`struct HT { T t; operator T*() { return &t; } }; return k ? y : HT(4);`, which really returns a pointer into a dead
temporary (and `return k ? y : h;` into a local), the same way `h.get()` / `HG(4).get()` member calls are refused
(IsFrameOwnedBorrowingMemberCall, MainListener_Statements.cpp). TERNLEFT5 r2 tried exempting every conversion
operator there and was backed out as a safety hole. Accepting the PC form needs a ruling on borrowed results of
conversion operators on frame-owned receivers.
