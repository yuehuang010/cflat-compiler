# Tuple leftovers after tuplit (2026-10-31)

All PRE-EXISTING (master same), found by the tuplit Opus / Sol reviews. Repros: scratch/repro_keep/tuplit/. The element-conversion
acceptance gaps (nullptr, simd splat, unique element, generic identity result, wider tuple ctor via `new`) landed with tuplefts.
- Tuple-typed lambda parameters do not parse.
- A by-value tuple parameter holding a type with a destructor never runs that destructor; struct temporaries in a call-argument tuple
  literal are not destroyed at scope end (r2s04 gd=0).
- `(unique<D>,int) t = (u,4)` with `u` a unique local double frees (rc 133, master same; tuplefts review 1 uniq_local.cb). `new S((nullptr,4))`
  with an `(int*,int)` ctor is refused "cannot find the type ''". Assignment `t = idg((3,4))` and declaration both stay refused (non-literal
  tuple needs an exact type - ruled 2026-11-01 at tuplefts).
- `string s = nullptr;` segfaults at run time (rc 139, master same); the tuple element form `(string,int) t = (nullptr,4)` is refused.
- Owned unique-element shapes the plain declaration accepts are refused "from a borrowed value" (safe side, master same):
  `((D*)new D(), 4)`, `((new D()), 4)`, `(idg(new D()), 4)` (tuplefts review 3, r3_castnew / r3_parennew / r3_gennew).
- `D d; D* q = &d; unique<D> u = move q;` runs to rc 134 (master same); the tuple element form matches it.
