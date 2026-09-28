# Unscoped C++ enum passed to a C++ `double` parameter is refused

Pre-existing (found 2026-09-27 by the fix/enum-cond review): `al3.takeD(u)` with `enum UE { ... } u;` and
`void takeD(double)` is refused; clang++ accepts (unscoped enum -> integral promotion -> floating conversion).
Scoped enums are correctly refused. Fix direction: in C++ candidate scoring, an unscoped enum argument converts
like its underlying integer (including to floating parameters). Probes: /Users/felixhuang/source/cflat-fix-enum-cond/scratch/revAL/.
