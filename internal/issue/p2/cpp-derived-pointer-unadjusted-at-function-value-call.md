# A derived C++ pointer passed through a `function<>` value is not base-adjusted

Found 2026-09-23 while fixing the reference-return-at-return issue (macOS arm64, Release). Pre-existing,
silent wrong value.

## Summary

`function<int(R*)> f = takeR; f(b)` with `b` a `B*` (`struct B : L, R`, R NON-primary) passes B's
address unadjusted. Direct calls to a CFlat function are adjusted since
fix/cpp-reference-return-at-return (the M6 argument arm in `CreateOverloadedFunctionCall` used to
run for C++ callees only).

## Repro

scratch corpus `rr_a07.cb`: exits 1, expected 0.

## Fix direction

Apply the same derived-to-base argument adjust in the closure / function-pointer invoke path.
