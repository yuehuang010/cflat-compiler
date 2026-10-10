# `i + 1.0` / `f + 1.0` (variable plus double literal) is computed and typed as float

C/C++: an unsuffixed floating literal is double, so `f + 1.0` with float f is double and `i + 1.0` with int i is
double. CFlat computes these in float, which shows up at C++ overload ranking / template deduction (float picked,
clang double) and may lose precision. Pre-existing on master; found by T36 round 3 (2026-10-05).

## Repro

```
// header: inline int pick(float) { return 2; } inline int pick(double) { return 3; }
int i = 1; float f = 1.0f;
cppt.pick(i + 1.0);   // CFlat 2, clang 3
cppt.pick(f + 1.0);   // CFlat 2, clang 3
```

## Fix direction

Check CFlat's own literal typing rule first (is `1.0` deliberately float in CFlat? doc/LANGUAGE.md); if CFlat
follows C, the arithmetic result is double. If CFlat deliberately types `1.0` as float, this needs a ruling.
