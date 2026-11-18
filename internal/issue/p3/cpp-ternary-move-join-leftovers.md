# Ternary move-join leftovers after T249 and T263 (lowered-struct and C++ shapes)

## Lowered-struct ternary leftovers after T249 (repros scratch/repro_keep/t249_rev/cases3/, clang twins .cpp)
- (counts differ from clang; no leak on current binaries) `take2T(h2, k ? mkHT() : move h1)` with a later sibling copy throwing (e3.cb): worktree/master construct the `mkHT()` join before copying `h2` (rv.C 4/4, th.T 3/3); clang copies `h2` first, throws, and never evaluates `mkHT()` (rv.C 3/3, th.T 2/2). The join owner is already registered in the resume block. Root cause is delayed by-value argument materialization after later arguments; preserve clang's evaluated order.
- (pre-existing) `k ? move h1 : h2;` as an expression statement does no move/copy and zeroes h1 (disc.cb).
- (pre-existing) by-value first-argument unwind leak (e2b.cb, e2c.cb), master the same.

## From T263 review 1 (pre-existing on master; Host = CFlat struct with a C++ field; repros scratch/repro_keep/t263_rev/)
- (T264 review 2) `rq.cref(k ? mkC(1) : mkC(2))` does one extra move+destroy vs clang, but the direct sibling `rq.cref(mkC(1))` has the same mismatch; this is CFlat-return materialization, not a ternary-only failure.
  Repros scratch/repro_keep/t264_rev/.

## T288 review 1 leftovers (pre-existing on master, not fixed by T288)
- Source reads stay allowed after a ternary move join (onearm_read.cb, bare.cb), while a direct move refuses them (direct.cb).
- `flat_host.cb` does one extra move/dtor versus clang; its Host aggregate/native unique-field path is owned by another run.
- `derived.cb`'s D/C xvalue join is accepted (derived/base reference join, C&& move-assignment).
- Declaration `T z2 = k ? move a : move b;` with const arms now copy-constructs the selected source, which remains unchanged, matching clang++ -std=c++20.
- `nested_outer.cb` now joins directly without the extra move/dtor and matches clang++ -std=c++20.
- Const by-value parameter (`void f(const rv.C a)`): CFlat keeps no const on by-value params, so `z = k ? move a : move b` with `a` such a parameter moves on T288 (master copied); master's explicit `z = move a` already moves it. Needs const carried on parameter signatures.
- Const-host field join `z = k ? move s.a : move s.b` (s const): clang and master copy-assign and keep both sources; T288 r3 falls back to master's path (leg 99015/99016).

## Ternary move-join open items after the ternmv round 3-4 review (fix/ternmv)
- OPEN: `b4_r_c1_rev` (`C h(bool k) { const C a; C b; C c = k ? move b : move a; return c; }`, then `z = h(k)`): the const-arm value matches clang (z=22), but the return of local `c` moves once more and dtor +1 (clang: copy, no move; move-assign into z). Master differs too (r=-1). Return-of-local count only; not in the decl path.
