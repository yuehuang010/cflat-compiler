# C++ user-defined conversion rank is a per-candidate total, not compared per argument

## Summary

Overload ranking counts user-defined conversions as one total per candidate
(`Ranked::userConversions`, cflat/LLVMBackend_Overloads.cpp, non-perfect tier). C++ compares
candidates argument by argument: a candidate better on one argument and worse on another is
ambiguous. cflat silently picks the candidate with the lower total. Wrong-accept only.

A second, diagnostic-only facet: when ONE argument's own conversion is ambiguous (two viable
conversion operators, `CxxConversionOperatorTo` returns empty), the candidate just becomes
non-viable and the call reports "no overload of ... matches the given arguments" where clang
says "conversion is ambiguous".

## Repro

```cpp
struct P2 { int v; P2() : v(1) {} operator int() const { return 5; } };
inline int mix(int a, double b) { return 1; }
inline int mix(const P2& a, float b) { return 2; }
```

```cflat
P2 p = default; double d = 1.0;
return mix(p, d);   // cflat: 1. clang: call to 'mix' is ambiguous.
```

Second facet: `amb(double)` / `amb(char)` called with a class declaring `operator int` and
`operator bool`.

## Fix direction

Keep per-argument conversion ranks on `Ranked` and refuse (tiedOut) when neither candidate is
at least as good on every argument. Surface the recorded per-argument ambiguity in the
no-overload diagnostic.
