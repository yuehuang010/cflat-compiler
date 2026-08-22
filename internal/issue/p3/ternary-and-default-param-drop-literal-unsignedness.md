# Ternary results and default parameter values still sign-extend a u-suffixed literal

Filed 2026-08-21 from review of the unsigned-literal fix (fix/unsigned-literal-widening), which
fixed the literal/widening/int-to-fp paths but not these two sibling sites. Pre-existing: identical
on master and the fix branch.

## Repro

```cflat
i64 t = true ? 4294967295u : 1;          // -1, expected 4294967295
i64 dflt(i64 v = 4294967295u) { return v; }
printf("%lld\n", dflt());                 // -1, expected 4294967295
```

## Root cause

`cflat/MainListener_Expressions.cpp` (ternary type balancing, ~3525-3534) and the default-parameter
materialisation call the 2-arg `Upconvert`, which assumes a signed source, instead of the overload
that takes the source's `IsUnsignedInteger() != -1`.

## Fix direction

Thread the arm's / default value's signedness into those `Upconvert` calls, as the aggregate
initializer sites now do. Add both lines above as value legs in `Test/test_operators.cb`.
