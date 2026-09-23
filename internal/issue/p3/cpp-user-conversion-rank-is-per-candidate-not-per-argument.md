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

## Measurements 2026-09-23 (codex attempt, fix/cpp-user-conversion-rank-per-argument, discarded)

The repro is confirmed on 163eeeef (`mix(P2, double)` returns 1; clang: ambiguous). Three
per-argument ranking edits to `Ranked` in the non-perfect tier did not change the measured
result - the winning path is NOT the `userConversions` total: the resolver splits `perfect`
and `possible` candidates before ranking, and the pick is decided earlier. Two further cells:
`cr_equal_user_exact` (equal user conversions on one argument, exact vs standard on the
other) picks the standard-conversion candidate (6, expected 5), and `cr_float_exact` (a
`double` overload vs a `float` one called with a double variable) picks float (11) - so the
shared resolver's standard-conversion comparison is suspect too, independent of user
conversions. Second facet: the per-argument ambiguity from CxxConversionOperatorTo can be
appended to the no-overload detail cheaply (both ScoreCxxConversionOperatorArgument sites).
Next attempt: trace ComputeOverloadFunction on cr_mixed with lldb to find which tier picks
candidate 1 before touching the ranking. Probe corpus was scratch/cr_*.cb (not kept).
