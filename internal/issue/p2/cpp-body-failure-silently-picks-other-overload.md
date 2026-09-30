# C++ member / constructor whose body fails to instantiate silently loses to another overload

Found by the R4 review round 1 (perf timebox 2026-09-29, member bodies on call). Same on master.

## Symptom
```cpp
namespace rv {
template<class T> struct P { int n = 0; int f(const T& x) { return T::nope; } int f(long x) { return 9; } };
template<class T> struct Q { T v; Q(const T& x) : v(x) { T::nope; } Q(double x) : v(11) {} };
}
```
`rv.P<int> p = default; p.f(i)` with `int i` runs `f(long)` (exit 9); `rv.Q<int>(i)` runs `Q(double)`
(exit 11). clang++ picks `f(const int&)` / `Q(const int&)` by overload resolution and then rejects the
program ("type 'int' cannot be used prior to '::'"). Repro: scratch/repro_keep/r4rv/p4 (rv.h, b.cb).

## Root cause
Overload ranking never looks at bodies (correct), but when the winning candidate's body fails at the
demand check, the call site retries without it: LLVMBackend_Overloads.cpp ~3072-3081 (member retry) and
the SelectCxxConstructor loop in LLVMBackend_CInterop.cpp (~20779 on the R4 branch).

## Fix direction
Ruling R4 (2026-09-29): a body failure is an ERROR AT THE USE SITE relaying clang's text, never a
silently different overload. Drop the retry for body failures: keep the winner, LogError at the call.
Signature-level failures (declaration cannot be formed) still exclude the candidate before ranking.
