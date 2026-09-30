# Deferred special members: harvests between the cutoff and torch size pay more cold

R1 (perf timebox 2026-09-29) defers the implicit and defaulted special members of harvested C++ records until first projection. Harvests below 64 pending records stay eager (kMinDeferredRecords in CClangExtract.cpp ComputeCxxAbi). The maintainer ruled that cutoff as is, and said a later perf review may make it adjustable.

## Measured (R1 review, fresh cache, instructions retired, 2 runs each)
| Program | pending records | master | R1 | change |
| --- | --- | --- | --- | --- |
| Test/test_cpp_interop_template.cb | 124 | 168.0 / 168.2 G | 170.6 / 170.4 G | ~+1.4% |
| Test/test_cpp_interop.cb | 84 | 331.6 / 332.3 G | 333.9 / 340.0 G | +0.7..+2.3% |
| torch train.cb | 2331 | 69.45 G | 64.51 G | -7.1% |

## Cause (likely)
"Used" means projected, and projection is wider than construction. A record reached only as the return type of another function in the namespace also gets its own completion chunk. Example: the review's p11a uses only AA and BB, yet it also completes rvp.Agg and rvp.Movable.

## Directions
1. Measure the 64..~500 pending range on real headers and set the cutoff from the data (or make it adjustable - maintainer: future perf review).
2. Complete a record on first construction / copy / destruction / by-value ABI need instead of on projection. Every fact reader must still complete first; the R1 review audited those readers.
3. Add one interop fixture compile to the perf check so this range stays visible.
