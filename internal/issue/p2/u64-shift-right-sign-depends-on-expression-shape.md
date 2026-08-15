# u64 right shift changes meaning after a nested left shift

## Summary

For `u64` operands, an unsigned right shift is not consistently logical. A right shift applied
to a nested `<<` expression sign-extends, while the same value held in a named `u64` local shifts
logically. The result depends on expression shape.

## Minimized repro

```cflat
extern int main()
{
    u64 v = (u64)0xFF880000;
    u64 s = v << 32;
    printf("named=%d inline=%d\n", (int)(s >> 48), (int)((v << 32) >> 48));
    return 0;
}
```

Measured output is `named=65416 inline=-120`. The named spelling is the logical unsigned result
(`0xFF88`); the nested spelling sign-extends the high 16-bit value when it is cast to `int`.

Both operands above are compile-time constants, so `main`'s IR carries no shift at all - it
passes the already-folded `i32 65416` and `i32 -120` to `printf`. Make the value non-constant
and the same divergence survives to runtime:

```cflat
extern int main(int argc, char** argv)
{
    u64 v = (u64)0xFF880000 * (u64)argc;   // argc-derived so nothing folds
    u64 s = v << 32;
    printf("named=%d inline=%d\n", (int)(s >> 48), (int)((v << 32) >> 48));
    return 0;
}
```

Also prints `named=65416 inline=-120`, and its `--out-lli` IR shows the two spellings taking
different opcodes:

```llvm
  %2 = lshr exact i64 %1, 48     ; named u64 local
  %5 = ashr exact i64 %4, 48     ; nested (v << 32) >> 48
```

## Root cause

Shift-opcode selection, not constant folding: the operand type carried by a nested `<<`
sub-expression is not the unsigned type of its operand, so the `>>` above it is emitted as
`ashr` instead of `lshr`. Constant folding merely mirrors the same wrong signedness when the
operands happen to be literals.

## Fix direction

Propagate the unsigned operand type through nested shift sub-expressions so `>>` on any `u64`
expression emits `lshr`, and keep the constant folder in step with the emitted opcode. Add value
tests for BOTH a folded and a non-foldable operand - a constants-only test cannot tell `lshr`
from `ashr`, because the fold produces the answer without either.

The GDI canvas wheel code now uses an explicit signed-half idiom before shifting, so fixing the
compiler will not change that UI behavior.
