# A '?:' of a derived and a base C++ pointer is not base-adjusted

Found 2026-09-23 while fixing the reference-return-at-return issue (macOS arm64, Release). Pre-existing,
silent wrong value.

## Summary

With `struct B : L, R` (R the NON-primary base), `R* r = k > 0 ? b : o;` (b a `B*`, o an `R*`)
stores B's address unadjusted, so `r.rv` reads L's storage. Same for `return k > 0 ? b : o;` from
an `R*` function. The single-value forms are adjusted at declaration, `=`, return and CFlat call
arguments.

## Repro

scratch corpus `rr_q06.cb` (declaration) and `rr_p16.cb` (return): both exit 1, expected 0.

## Fix direction

Adjust each arm to the join's destination class inside the '?:' lowering (per-arm, before the
phi), null-preserving via `EmitCxxBaseAdjust`.
