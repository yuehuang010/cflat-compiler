# Tuple leftovers after tuplit (2026-10-31) - residue after TUPLEFT (2026-11-05)

Repros: scratch/repro_keep/tuplit/ (main checkout).

Not reproduced on master 2026-11-05 (TUPLEFT): by-value tuple parameter dtor (r2s04 gd=1 at -O0/-O2/-g; ctor/dtor counts
balance for call literals, named tuples, lambda params, methods, generic params); pinned by the tdp_* legs in test_generics.

## Still open (pre-existing, master same)

- Assignment `t = idg((3,4))` and declaration both stay refused (non-literal tuple needs an exact type - ruled 2026-11-01 at tuplefts).
