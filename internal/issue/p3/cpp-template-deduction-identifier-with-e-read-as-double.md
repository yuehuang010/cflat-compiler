# C++ template deduction reads `1 + abcer + 1` as a double literal (identifier contains 'e')

The literal-identity check for a C++ call argument classifies the argument TEXT; an identifier containing `e`/`E`
next to digits makes the whole expression look like a floating literal, so template deduction picks double.
Pre-existing on master; found by T36 round 2 (2026-10-05).

## Repro

```
// header: template<class T> int kind(T) { return std::is_same_v<T, double> ? 3 : std::is_pointer_v<T> ? 1 : 0; }
int abcer = 2;
cppt.kind(1 + abcer + 1);   // CFlat 3 (double), clang 0 (int)
```

## Fix direction

Propagate the expression's source type through typed expression evaluation into C++ call construction; never
classify by text. T36 rounds 2-3 tried a text split at top-level operators and regressed 20 cells
(`1e3+(1+1)`, `1e3+(c?1:2)`, `1.0+x` with float x -> float instead of double, where master and clang
agree on double); T36 round 4 reverted it. Probes: scratch/repro_keep/t36_rev4/.

## Also (T36 review 4, 2026-10-06, master same)

- Pointer form: `kind(1 + t27CharPointer)` (pointer whose identifier contains 'e') deduces double / double* and is
  refused; clang 3 (char*). `1 + cP + 1` without an 'e' works after T36.
- Float-suffix literals: `1.0f + 1`, `1.5e-3f - 1`, `1E3F + 1`, `0x1p3f + 1` deduce double, clang float.
- Other literal-text mismatches: `(1e3)+(1+1)`, `-(1e3)+1`, `c ? 1e3 : 1`, `(float)x + 1.0`, `1e3 + 1UL`,
  `1e3 + 1LL` pick float (clang double); `0x1p3` and comparisons like `1e3 > 1` refused (clang: double / bool).
