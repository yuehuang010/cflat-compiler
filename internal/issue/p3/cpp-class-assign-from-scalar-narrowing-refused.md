# C++ class assignment from a narrowing scalar is refused; ambiguous C++ copy-init has a misleading hint

Found by T92 review r2 (2026-10-10), pre-existing on master. Probes: scratch/repro_keep/t92/rev_t92r2/
(run.sh; each case prints master | fix | clang).

1. `rv.SU e = default; e = i;` (int i, SU has SU(unsigned)) and the EF variant are refused
   (c10_assign.cb, c25_ef_assign.cb). clang accepts: implicit conversion through the converting ctor,
   then the implicit move-assignment operator. T92 made copy-init `rv.SU s = i;` accept the same
   narrowing conversion (allowScalarNarrowing), so assignment now lags copy-init.
   Fix direction: pass allowScalarNarrowing at the C++ class assignment RHS conversion site.
2. Ambiguous copy-init (two narrowing ctors, e.g. SB(signed char) + SB(bool) from int;
   c04/c06/c20/c21/c23) is refused correctly, but with "cannot initialize C++ class 'rv.SB' from this
   expression; use 'rv.SB(args)'" - the suggested direct form is ambiguous too. Say "ambiguous" and
   name the candidates like clang.
