# Box<Lambda<int(int)>>.set(<capturing lambda literal>) then b.v(1) crashes

Summary: storing a capturing lambda literal into a generic Lambda<> field through a method parameter, then calling the field,
exits 138 (bus error) on master and after T185. Direct field assignment / brace init of the same lambda works (T185 r2 probes).
Likely the closure environment is not retained across the by-value T parameter (lifetime of the capture block), but unconfirmed.
Repro: scratch/repro_keep/t185_rv2/ (probe.sh case for Box<Lambda>.set; found by T185 Opus r2, 2026-10-14).
