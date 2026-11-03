# Tuple leftovers after tuplit (2026-10-31)

All PRE-EXISTING (master same), found by the tuplit Opus / Sol reviews. Repros: scratch/repro_keep/tuplit/.
- Tuple-typed lambda parameters do not parse.
- A by-value tuple parameter holding a type with a destructor never runs that destructor; struct temporaries in a call-argument tuple
  literal are not destroyed at scope end (r2s04 gd=0).
- simd splat element `(simd<float,4>,int) t = (1.0f, 4)` refused (scalar `simd<float,4> v = 1.0f` accepts).
- `nullptr` element into `(int*,int)`, `(unique<D>,int) t = (new D(), 4)`, `typedef (int,int) P;`, `(i64,int) t = idg((3,4))` refused.
- A single wider `new S((3,4))` ctor refused (direct ctor call converts).
