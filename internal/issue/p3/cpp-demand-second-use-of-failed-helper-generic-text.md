# Second use through an already-failed C++ helper is refused without clang's text

Found by the R4 review round 3 (perf timebox 2026-09-29).

## Symptom
```cpp
namespace rv { template<class T> struct W { int h() { return helper() + 1; } int h2() { return helper() + 2; }
                                            int helper() { return T::nope; } }; }
```
`w.h()` then `w.h2()`: both are refused at their own call, but h2 reads "... (clang: failed to instantiate
'rv::W<int>::h2')" while h relays clang's "type 'int' cannot be used prior to '::' ...". Clang does not
re-diagnose the already-invalid helper, so the h2 check has no new text. Repro: Test/errors/err_cpp_sink_blame_precise.cb
(sbl::Shared legs).

## Fix direction
When MaterializeReachable (CxxIncrementalGroup.cpp) meets an invalid `next`, relay the text recorded for
it at the earlier failure (poison it with the h check's text when the helper appears in that check's
instantiation notes, or keep a decl -> first-diagnostic map for invalid instantiations).
