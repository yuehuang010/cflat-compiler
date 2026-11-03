# A C-style cast to a C++ class through a converting constructor is refused

Summary: `(T)a` where T is an imported C++ class with a converting constructor from a's type (mutable or const source) is
refused by CFlat; clang++ -std=c++20 accepts it as a direct-initialization (C-style cast = static_cast here).
Repro: CTOREXP Sol review probes conv_cast_mut.cb / conv_cast_const.cb (scratch/repro_keep/ctorexp/rev_ctorexp_sol/),
clang exits 51 / 41; master and the CTOREXP fix both fail to compile.
Found: CTOREXP Sol review (box 7 run 2, 2026-10-31), PRE-EXISTING.
Fix direction: route an explicit cast to a C++ class type through the C++ converting-constructor path as direct-init
(explicit constructors allowed), the same way the same-class cast is handled after CTOREXP.
