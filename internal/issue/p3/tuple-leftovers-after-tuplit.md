# Tuple leftovers after tuplit (2026-10-31) - residue after TUPLEFT (2026-11-05)

Repros: scratch/repro_keep/tuplit/ (main checkout).

Not reproduced on master 2026-11-05 (TUPLEFT): by-value tuple parameter dtor (r2s04 gd=1 at -O0/-O2/-g; ctor/dtor counts
balance for call literals, named tuples, lambda params, methods, generic params); pinned by the tdp_* legs in test_generics.

## Still open (pre-existing, master same)

- Assignment `t = idg((3,4))` and declaration both stay refused (non-literal tuple needs an exact type - ruled 2026-11-01 at tuplefts).
- Owned unique-element shapes the plain declaration accepts are refused "from a borrowed value" (safe side, master same):
  `((D*)new D(), 4)`, `((new D()), 4)`, `(idg(new D()), 4)` (tuplefts review 3, r3_castnew / r3_parennew / r3_gennew).

## Pre-existing repros retained after TUPSAFE round 2

- Two scalar by-value `unique` arguments that alias the same slot double-free (R133 on the mandated master exe and round-3 branch): `import "unique.cb"; int dt = 0; struct D { ~D() { dt = dt + 1; } }; int kk5(unique<D> a, unique<D> b) { return 0; } int main() { unique<D> u = new D(); kk5(u, u); return dt; }`; replacing the call with `unique<D>* pu = &u; kk5(u, *pu);` reproduces the same failure. This round is limited to tuple-element sequencing; scalar call-argument sequencing is separate work.
- A raw pointer borrowed on only one branch then moved into unique still aborts (R134): `import "unique.cb"; struct D { int v = 0; ~D() { } }; int main() { int c = 0; D x = default; D* p = &x; if (c) p = new D(); unique<D> u = move p; return 0; }`.
- A borrow stored in a field then moved into unique still aborts (R134): `import "unique.cb"; struct D { int v = 0; ~D() { } }; struct H { D* p = default; }; int main() { D x = default; H h = default; h.p = &x; unique<D> u = move h.p; return 0; }`.
