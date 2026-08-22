# Equal-width sub-int mixed-signedness ops skip C integer promotion

Filed 2026-08-21 from review of the unsigned-literal fix (fix/unsigned-literal-widening).
Pre-existing: identical on master and the fix branch.

## Repro

```cflat
i8 nc = -1; u8 uc = 3;
printf("%d %d\n", nc < uc, nc / uc);   // prints 0 85; C prints 1 0
```

## Root cause

`PromoteToInt` does not apply C integer promotion to i8/i16 operands, so the usual-arithmetic-
conversions rule in `CreateOperation` (`cflat/LLVMBackend_VariablesAndIR.cpp`, `resultIsUnsigned`)
sees an 8-bit vs 8-bit pair and picks unsigned. C promotes both to `int` first, so the compare and
the division are signed.

## Fix direction

Promote i8/i16/u8/u16 operands to i32 (signed) before applying the width/signedness rule, then
narrow the result back where the language requires it. Add legs to `Test/test_operators.cb`
asserting `1` and `0` for the repro.
