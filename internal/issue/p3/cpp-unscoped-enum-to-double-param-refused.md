# Unscoped C++ enum passed to a C++ `double` parameter is refused

Pre-existing (found 2026-09-27 by the fix/enum-cond review): `al3.takeD(u)` with `enum UE { ... } u;` and
`void takeD(double)` is refused; clang++ accepts (unscoped enum -> integral promotion -> floating conversion).
Scoped enums are correctly refused. Fix direction: in C++ candidate scoring, an unscoped enum argument converts
like its underlying integer (including to floating parameters). Probes: /Users/felixhuang/source/cflat-fix-enum-cond/scratch/revAL/.

Related (T57 review, 2026-10-06, pre-existing): native calls accept an imported C++ SCOPED enum value
(`SC` variable, and after T57 also `(SC)200`) into an int / u8 / C++ unscoped-enum / native-enum
parameter; clang++ refuses implicit conversion from a scoped enum. Calls into C++ refuse it on master
and T57. Belongs to the same scalar-conversion-table ruling.
