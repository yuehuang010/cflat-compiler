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

RULED 2026-11-04 (maintainer, reopening the deferral): C++ TEMPLATE DEDUCTION from any argument expression containing an
unsuffixed floating literal deduces double, extending LANGUAGE.md's "generic deduction is the exception" line (bare literals
already do). C++ overload RANKING keeps CFlat's narrowest literal rule, the same as a CFlat overload set (bridge
transparency): `cppt.pick(i + 1.0)` / `pick(f + 1.0)` stay `pick(float)`, a deliberate clang difference. Test consequence:
the T439 `-(1e3) + 1` C++ template leg becomes a clang-parity leg (double); the pick(float) shapes are pinned as by-design.

DEFERRED 2026-10-13 (maintainer): `1.0` is float by design (doc/LANGUAGE.md, narrowest literal typing); whether C++ call
arguments should type unsuffixed floating literals as double is deferred - no work until reopened.

T439 (2026-10-23) re-measured the text-classification shapes from the retired issue
cpp-template-deduction-identifier-with-e-read-as-double: all match clang on master except `-(1e3) + 1` at a C++ template
call (CFlat float, clang double) - the same deferred literal-typing question as this issue.
