# C++ const global with a side-effect initializer read as a folded constant, initializer never runs

Found by the ST4 review (2026-10-01), pre-existing on master. Repro: copy of the reviewer's probe
in scratch/repro_keep/st4_preexisting/dynamic.cb.

```cpp
inline int counter = 0;
inline const int dyn = (++counter, 27);
```

CFlat reads `dyn` as 27 but `counter` stays 0; clang++ gives 27 and 1. Same mechanism as the
variable-template case fixed on fix/std-entity-lookup (ST4): only constexpr / constant-initialized
variables without side effects may be folded; others must bind the live object and run its
dynamic initializer.
