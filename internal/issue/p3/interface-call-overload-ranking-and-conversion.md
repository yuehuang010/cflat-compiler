# Interface-call overload residue after IFACEOVL + IFCONST

IFACEOVL (483f3b83d) landed: interface method calls rank overloads exactly as a direct class call. IFCONST (2867b87d2) landed:
an unrelated object pointer to `{f(Pt*), f(Qt*)}` is refused on the interface path like the direct class call; on a const
receiver only const-qualified overloads are viable on both paths (C++ rule); C++ pointer parents keep only pointee const.
Repros scratch/repro_keep/ifaceovl/; direct = the same call on a class receiver.

Decision at landing (main session, 2026-10-29): native CFlat strict call conversions win over the ruling's "=2" example.
`{f(int); f(double) const}` with `x.f(1)` on a const native receiver is REFUSED (the only const candidate needs int -> double,
which CFlat never does implicitly at a call), naming the non-const f(int); `x.f(1.0)` gives 2. C++ classes keep C++ conversions.

Remaining:
- ConstMask hole: a `const C*` to a native class still calls a single non-const native method (no overload set involved).
  Closing it needs const annotations on core containers first (core/list.cb and friends call through const receivers).
- Pre-existing (clang: ambiguous): `nullptr` to `{f(Pt*), f(Qt*)}` and `int -3` to `{f(u32), f(double)}` pick the first; a
  `Col : u8` enum argument to `{f(u8), f(Col)}` picks f(u8); `-(1)` picks u8, `(1.0)` picks float.
- Pre-existing: a single-slot interface `f(double)` accepts an int argument (2); the same direct native call is refused.
- Pre-existing: native `const NH* hp; hp->p->v = 5` is refused ("reached through a pointer to const"); clang accepts - the
  native field-write path still carries the parent's const through a pointer field (the C++ path was fixed by IFCONST).
- Pre-existing: const twins `get()` / `get() const` on a native struct emit one body - const is not in the mangling
  (Test/errors/err_duplicate_function_same_line.cb T431ConstPair).
