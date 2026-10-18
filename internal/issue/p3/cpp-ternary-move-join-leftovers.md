# Ternary move-join leftovers after T249 and T263 (lowered-struct and C++ shapes)

## Lowered-struct ternary leftovers after T249 (repros scratch/repro_keep/t249_rev/cases3/, clang twins .cpp)
- (unwind leak) `take2T(h2, k ? mkHT() : move h1)` where a LATER sibling argument's copy throws: the join temp
  leaks (made 4, dt 3; clang 3/3) - e3.cb. Master refused the call. The join temp needs an unwind cleanup.
- (pre-existing) `k ? move h1 : h2;` as an expression statement does no move/copy and zeroes h1 (disc.cb).
- (pre-existing) by-value first-argument unwind leak (e2b.cb, e2c.cb), master the same.
  Sret CALL result as the first by-value argument with a throwing later sibling: `hv2(mkH(1), rq.thrower(x))`
  through rq.guarded leaks (dt 0, clang 1), master the same (reviewer repro scratch/rev_t264_opus_repro/T8.cb).
  The argument-join form (`hv2(k ? mkH(1) : mkH(2), thrower)`) is fixed by T264 round 2 (owner registered in the resume block, disarmed at the adopting call).

## From T263 review 1 (pre-existing on master; Host = CFlat struct with a C++ field; repros scratch/repro_keep/t263_rev/)
- `z = (k ? mkH(1) : mkH(2));` parenthesized copy-assigns and never destroys the join temp (bare form exact).
- `(k ? mkH(1) : mkH(2)).c.get()` -> "internal: nontrivial C++ receiver reached register-return temporary lowering".
- a one-move-arm ternary as a call argument (by value / const&) or method receiver copies; `a` not moved (clang moves).
- a deleted move-assign is not refused when the rhs is a temp (`z = k ? move a : b`, `z = DM(5)`); `z = move a` refuses.
- a parenthesized move arm `k ? (move a) : b` is not recognized (assignment and the T246 declaration form copy).
- (T264 review 2, unwind leak, master the same) a first argument that is a call result, not a bare join, leaks when a later
  sibling throws: `hv2(gid(mkH(1)), rq.thrower(x))`, `hv2(gid(k ? mkH(1) : mkH(2)), rq.thrower(x))`. A C++ class join
  passed to a C++ function (`rq.cref(k ? mkC(1) : mkC(2))`) does one extra move+destroy vs clang (no leak).
  Repros scratch/repro_keep/t264_rev/.

## T288 review 1 leftovers (pre-existing on master, not fixed by T288)
- Source reads stay allowed after a ternary move join (onearm_read.cb, bare.cb), while a direct move refuses them (direct.cb).
- nested_outer.cb and flat_host.cb do one extra move/dtor versus clang.
- derived.cb and convert.cb are refused as incompatible ternary types.
- Declaration `T z2 = k ? move a : move b;` with const arms moves the const sources on master (clang copy-constructs, sources untouched); not reached by the T288 assignment join, no leg.
- Const by-value parameter (`void f(const rv.C a)`): CFlat keeps no const on by-value params, so `z = k ? move a : move b` with `a` such a parameter moves on T288 (master copied); master's explicit `z = move a` already moves it. Needs const carried on parameter signatures.
- Mutable field join `z = k ? move s.a : move s.b` (s a mutable local): clang move-assigns (one source reads -1 after the join); master copy-assigns and keeps both sources. T288 r3 leaves it on master's path (legs 99017/99018 pin master). Pre-existing, not fixed by T288.
- Const-host field join `z = k ? move s.a : move s.b` (s const): clang and master copy-assign and keep both sources; T288 r3 falls back to master's path (leg 99015/99016).
