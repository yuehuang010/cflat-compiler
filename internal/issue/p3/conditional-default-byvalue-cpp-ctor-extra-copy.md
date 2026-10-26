# Conditional default passed to by-value C++ constructor adds copy and leaks

Summary: A by-value C++ constructor argument `c ? default : v` performs an extra copy and leaks an allocation.
Repro: `T391SinkV(T391Val v)` with `c ? default : v`, `c=true`; T459 repro path `scratch/repro_keep/box1022/t459/`.
Master vs expected: CFlat 21021; clang 20020.
Found: T459 r2 haiku review, 2026-10-24 box, PRE-EXISTING
