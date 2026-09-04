# `x != x` is false for NaN: float `!=` lowers to an ORDERED compare

Filed 2026-09-03, found while adding the `init_capacity` NaN poison leg.

## Summary

Float `!=` emits `fcmp one` (ordered-not-equal), so `NaN != NaN` is FALSE, and `NaN != 1.0`
is also false. C/C++ and LLVM's `fcmp une` say `!=` is UNORDERED: any comparison involving a
NaN is unequal. The result is that `!=` and `!(a == b)` disagree on NaN, and the idiomatic
NaN check `x != x` silently reports "not NaN".

## Repro

`scratch/q03u_nan_ne.cb` on master 94ec41c:

    double z = 0.0; double n = z / z;
    printf("ne=%d eq=%d noteq=%d\n", n != n, n == n, !(n == n));   // ne=0 eq=0 noteq=1  (C: 1 0 1)

`--symbol-dump-ir function:main` shows `fcmp one` for `!=` and `fcmp oeq` for `==`.

## Root cause

The float inequality lowering uses `CreateFCmpONE` (grep `CreateFCmpONE` / `fcmp one` in
cflat/*.cpp); `==` correctly uses `oeq`. The remaining relational operators (`<`, `<=`, `>`,
`>=`) are ordered in C too (false on NaN), so only `!=` is wrong.

## Fix direction

Emit `fcmp une` for `!=` (LLVM `CreateFCmpUNE`). Check the operator-overload fallback and any
constant-folded `!=` on float literals agree, and check whether `simd` float `!=` has the same
lowering. Legs in an existing float test (grep Test/*.cb for `isnan` or `0.0 / 0.0`): `n != n`
is 1, `n != 1.0` is 1, `1.0 != 2.0` is 1, `1.0 != 1.0` is 0. Then the `init_capacity` poison leg
in Test/test_core.cb can go back to the natural `pv != pv` spelling.
