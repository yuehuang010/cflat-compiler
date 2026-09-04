# switch never matches a negative case label when the operand is int

## Summary

`int k = -129; switch (k) { case -129: ... }` does not take the case on master. The label is
presumably materialised as an unsigned or differently-widened constant so the `switch` instruction
compares against the wrong value. Narrow operands used to mask this: `switch (-b8) { case -128: }`
matched on master only because `-b8` stayed `i8`; after 5691668 promotes it to `i32` the same
switch misses.

## Repro (master cb3f71b, measured 2026-09-03 by review probe scratch/q03m1_rev1_c.cb)

```
int k = -129;
int hit = 2;
switch (k) { case -129: hit = 1; break; default: break; }
return hit;   // 2 on master, clang gives 1
```

## Fix direction

Find where case labels are evaluated to `ConstantInt` for the LLVM `SwitchInst` (MainListener
switch handling); sign-extend/convert the label to the switch operand's type instead of
zero-extending, and add a leg in Test/test_basic.cb or the existing switch test with a negative
label on `int`, `i8` and a promoted unary result.
