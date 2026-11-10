# Ternary move-join leftovers after T249 and T263 (lowered-struct and C++ shapes)

## Lowered-struct ternary leftovers after T249 (repros scratch/repro_keep/t249_rev/cases3/, clang twins .cpp)
- (counts differ from clang; no leak on current binaries) `take2T(h2, k ? mkHT() : move h1)` with a later sibling copy throwing (e3.cb): worktree/master construct the `mkHT()` join before copying `h2` (rv.C 4/4, th.T 3/3); clang copies `h2` first, throws, and never evaluates `mkHT()` (rv.C 3/3, th.T 2/2). The join owner is already registered in the resume block. Root cause is delayed by-value argument materialization after later arguments; preserve clang's evaluated order.
- (pre-existing) `k ? move h1 : h2;` as an expression statement does no move/copy and zeroes h1 (disc.cb).
- (pre-existing) by-value first-argument unwind leak (e2b.cb, e2c.cb), master the same.
  Sret CALL result as the first by-value argument with a throwing later sibling: `hv2(mkH(1), rq.thrower(x))`
  through rq.guarded leaks (dt 0, clang 1), master the same (reviewer repro scratch/rev_t264_opus_repro/T8.cb).
  The argument-join form (`hv2(k ? mkH(1) : mkH(2), thrower)`) is fixed by T264 round 2 (owner registered in the resume block, disarmed at the adopting call).

## From T263 review 1 (pre-existing on master; Host = CFlat struct with a C++ field; repros scratch/repro_keep/t263_rev/)
- (T264 review 2, unwind leak, master the same) a first argument that is a call result, not a bare join, leaks when a later
  sibling throws: `hv2(gid(mkH(1)), rq.thrower(x))`, `hv2(gid(k ? mkH(1) : mkH(2)), rq.thrower(x))`. `rq.cref(k ? mkC(1) : mkC(2))` does one extra move+destroy vs clang, but the direct sibling `rq.cref(mkC(1))` has the same mismatch; this is CFlat-return materialization, not a ternary-only failure.
  Repros scratch/repro_keep/t264_rev/.

## T288 review 1 leftovers (pre-existing on master, not fixed by T288)
- Source reads stay allowed after a ternary move join (onearm_read.cb, bare.cb), while a direct move refuses them (direct.cb).
- `flat_host.cb` does one extra move/dtor versus clang; its Host aggregate/native unique-field path is owned by another run.
- `derived.cb`'s D/C xvalue join is accepted (derived/base reference join, C&& move-assignment).
- OPEN: `convert.cb` (X/C via `X::operator C() const`) stays refused with master's incompatible-types diagnostic; clang accepts it (true 121,21,22; false 22,21,-1). A conversion join must neither mark the X source moved nor zero it before the conversion runs.
- Declaration `T z2 = k ? move a : move b;` with const arms now copy-constructs the selected source, which remains unchanged, matching clang++ -std=c++20.
- `nested_outer.cb` now joins directly without the extra move/dtor and matches clang++ -std=c++20.
- Const by-value parameter (`void f(const rv.C a)`): CFlat keeps no const on by-value params, so `z = k ? move a : move b` with `a` such a parameter moves on T288 (master copied); master's explicit `z = move a` already moves it. Needs const carried on parameter signatures.
- Mutable field join `z = k ? move s.a : move s.b` (s a mutable local): clang move-assigns (one source reads -1 after the join); master copy-assigns and keeps both sources. T288 r3 leaves it on master's path (legs 99017/99018 pin master). Pre-existing, not fixed by T288.
- Const-host field join `z = k ? move s.a : move s.b` (s const): clang and master copy-assign and keep both sources; T288 r3 falls back to master's path (leg 99015/99016).

## Ternary move-join open items after the ternmv round 3-4 review (fix/ternmv)
- OPEN: `b4_r_c1_rev` (`C h(bool k) { const C a; C b; C c = k ? move b : move a; return c; }`, then `z = h(k)`): the const-arm value matches clang (z=22), but the return of local `c` moves once more and dtor +1 (clang: copy, no move; move-assign into z). Master differs too (r=-1). Return-of-local count only; not in the decl path.
- OPEN: const FIELD leaf in a move join (`const N* p; z = k ? move p->f : b;`, `C z = k ? move p->f : move b;`). `joinMoveConst` sees only bare const names of the declared class, so a const field leaf moves its source as master does (b2_cfield_one: master's value r=-1). Clang copies the const leaf (const C&& composite). Needs the const of field and deref leaves from the arm expression.
- OPEN: `b3_x_field_decl` (`C z = k ? move s.x : move b;`, X has `operator C`): z=100, clang 121, master the same. X/C field join in a declaration takes the converting path without a refusal.

OPEN (2026-11-09): a one-const-arm move join as a constructor argument inside an outer ternary arm,
`z = k ? C(k ? move a : move b) : move c`, makes one extra copy (cp2 dt5; master and clang cp1 dt4). Value and sources are
right, ctors and dtors balance. Repros: p_ctor_nest and q_asg in scratch/repro_keep/ternmv/.
