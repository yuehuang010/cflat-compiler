# Interface-call overload residue after IFACEOVL + IFCONST + IFERES

IFACEOVL (483f3b83d) landed: interface method calls rank overloads exactly as a direct class call. IFCONST (5344802ed) landed:
an unrelated object pointer to `{f(Pt*), f(Qt*)}` is refused on the interface path like the direct class call; on a const
receiver only const-qualified overloads are viable on both paths (C++ rule); C++ pointer parents keep only pointee const.
IFERES landed: an integer argument to a floating-point interface slot is refused like the direct native call
(Test/errors/err_iface_int_to_floating_arg.cb; accept-set iface_ifres_accept_* in Test/test_interface.cb).

Decision at landing (main session, 2026-10-29): native CFlat strict call conversions win over the ruling's "=2" example.
`{f(int); f(double) const}` with `x.f(1)` on a const native receiver is REFUSED (the only const candidate needs int -> double,
which CFlat never does implicitly at a call), naming the non-const f(int); `x.f(1.0)` gives 2. C++ classes keep C++ conversions.

Measured 2026-11-19 (IFERES). Paths: interface value, generic interface `IG<int>`, interface field reached through a
struct pointer (`hp->i.f(x)`), direct class call, implicit-this call inside the class, free function. Every cell below
gives the SAME answer on all six paths; only the clang column differs. Probe corpus scratch/iferes/ (gen.py, run.sh).

Measured picks (same on all six paths; return value = 1-based overload index):

| Cell | CFlat | clang |
|---|---|---|
| `nullptr` to `{f(Pt*), f(Qt*)}` | 1 | ambiguous |
| `-3`, `-(1)`, `int v = -3` to `{f(u32), f(double)}` | 1 (u32) | ambiguous |
| `1`, `-(1)` to `{f(u8), f(float)}` | 1 (u8) | ambiguous |
| `(1.0)` to `{f(u8), f(float)}` | 2 (float) | ambiguous |
| `Col v` to `{f(u8), f(Col)}` | 1 (u8) | 2 (Col) |
| `u8 v` to `{f(Col), f(u8)}` | 1 (Col) | 2 (u8) |

Remaining:
- ConstMask hole: a `const C*` to a native class still calls a single non-const native method (no overload set involved).
  Closing it needs const annotations on core containers first (core/list.cb and friends call through const receivers).
- OPEN, NEEDS MAINTAINER RULING (pinned by existing legs): `nullptr` to two pointer overloads picks the first declared on every
  path; clang (and the GENOVLR 2026-10-29 ruling text, "nullptr to two pointer overloads is ambiguous") say ambiguous.
  `struct Pt { int a; }; struct Qt { int b; }; int f(Pt* a) { return 1; } int f(Qt* a) { return 2; }` + `f(nullptr)` -> 1.
  Test/test_interface.cb `iface_ov_pointer_concrete_null` / `iface_ov_pointer_null` assert `f(nullptr)` over
  `{f(int*), f(char*)}` = 11 (the first); those legs pin the pick, so ambiguity needs a maintainer ruling.
- OPEN, NEEDS RULING: an unscoped `enum Col : u8` and `u8` tie as exact matches, so declaration order decides, on every path.
  `enum Col : u8 { R = 1, G = 2 }; int f(u8 a) { return 1; } int f(Col a) { return 2; }` + `Col v = Col.G; f(v)` -> 1
  (clang 2, exact). Reversed declarations `int f(Col a) {..2} int f(u8 a) {..1}` + `u8 u = 1; f(u)` -> 2 (clang 1:
  u8 -> Col is not implicit). No ruling covers enum-vs-backing-type ranking.
- OPEN (follows the 2026-10-29 decision; diverges from clang): int -> floating is not viable at a native call, so the integer
  overload is the only candidate. `{f(u32), f(double)}` with `-3`, `-(1)`, `int v = -3` -> f(u32) (clang ambiguous);
  `{f(u8), f(float)}` with `1`, `-(1)` -> f(u8) (clang ambiguous). Single `f(u8)` takes `-1` / `-(1)` (value 255, as clang);
  `300` is refused.
- Literal typing (DEFERRED 2026-10-13): `{f(u8), f(float)}` with `(1.0)` -> f(float) (clang ambiguous); single `f(float)`
  takes `(1.0)` (as clang) but refuses `double v = 1.0; f(v)` (clang accepts: narrowing at a call is refused natively).
- Pre-existing: const twins `get()` / `get() const` on a native struct emit one body - const is not in the mangling
  (Test/errors/err_duplicate_function_same_line.cb T431ConstPair).

BY DESIGN (main session, 2026-11-19; not open): native `const NH* hp; hp->p->v = 5` is refused ("reached through a pointer
to const") while clang accepts. That is the transitive-const design (internal/plan/cflat-const.md: "`const S s` with a
pointer field: `*s.p = 5` refused too (transitive). C++ is shallow here"; doc/LANGUAGE.md "const is transitive"), pinned by
Test/errors/err_const_through_pointer.cb (`box.q->x = 2` on a `const C02Box`). Measured refused, clang accepts: `hp->p->v = 5`,
`+= 5`, `++`, `hp->pq->q->v = 7`, `(*hp->p).v = 11`, `const GH<V>* gp; gp->p->v = 9`, `const NH h; h.p->v = 8` / `h.p->v++`,
const method writing `p->v`.
