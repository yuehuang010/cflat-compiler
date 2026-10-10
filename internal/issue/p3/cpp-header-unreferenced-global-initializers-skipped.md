# C++ header globals the program never references do not run their dynamic initializers

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

## Narrowed 2026-10-05 (T44)

Direct reads now match clang (non-inline static const, namespace const with a call initializer, class static
const, constexpr folded: `27 29 31 37 41 calls=3`). What remains: dynamic initializers of header globals the
program never REFERENCES do not run (clang `calls=3`, CFlat `calls=0` - lazy import only binds used globals).
Probes: scratch/repro_keep/t44_pre/dyn_constctx.cb, dyn_matrix.cb. Needs a ruling-shaped decision: run every
imported header's dynamic initializers (C++ semantics, costs eager harvest) or document lazy-init as CFlat's rule.

## Deferred to p3 (maintainer, 2026-10-09) - not a blocker, for later

Probe 2026-10-09 (scratch/q8/, master Release vs clang++ -std=c++20):
| | CFlat | clang++ |
|---|---|---|
| `import cpp "s.cpp"` source: side-effect global + global object ctor | both run | both run |
| header global the program reads (`q8.hdr_ref`) | initializer runs | runs |
| header `inline` global never referenced | NOT run | runs |
| header `inline` object with side-effect ctor, never referenced | NOT run | runs |
Only unreferenced header inline globals differ (lazy import binds used globals only). Exposure:
header-only self-registration (inline registrar objects). libc++ / MSVC keep iostream init in the
runtime library, so `<iostream>` is unaffected. Fix later = run dynamic initializers of every
harvested header global (compile-time cost, gated by the test_libs cold/warm budget).
