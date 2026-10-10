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
(`1e3+(1+1)` -> float); see p2/cpp-pointer-arithmetic-result-type-not-propagated.md status section.
