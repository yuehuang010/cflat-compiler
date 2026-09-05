# Narrowing ruling not enforced at extern C call sites - `u8` prototype parameter lowers as i32

Bucket: full mode (adds a rejection at extern call sites; needs an accept-set over the C-interop
tests). Filed 2026-09-04 by the q08 review; pre-existing, not a regression of 680d9e8a.

## Summary

`extern int putchar(u8 c); int n = 300; putchar(n);` compiles pre and post q08 and emits
`call i32 @putchar(i32 %0)`: an extern prototype's `u8` parameter is not lowered as `u8` at all,
so neither overload scoring nor `LowerByValueArg` ever sees a narrowing, and the 2026-09-04
ruling ("no implicit integer narrowing at a call argument") is silently bypassed for every
extern C call. `extern void exit(u8); exit(n)` with `n = 300` exits 44.

## Fix direction

Decide first whether extern C prototypes should follow the CFlat call rule (recommended: yes,
the prototype spells `u8`, and C's own promotion to int is an ABI detail, not a licence to
narrow) or are exempt as "C territory". If enforced: run `ArgumentNarrowsParameter` against the
prototype's declared TypeAndValue at the extern call path (cflat/LLVMBackend_ControlFlowAndFunctions.cpp
LowerByValueArg / the extern recipe), keeping the i32 ABI lowering. Accept-set: every extern
call in Test/test_c.cb, Test/test_win*.cb, example/ that passes an `int` to a `char`/`unsigned
char` prototype (auto-extern from clang JSON spells those as i8/u8) - measure before guarding;
this is likely to false-reject real C idioms (`putchar('a' + i)`), so the literal/expression
width rule needs care. Probe both `extern` written by hand and auto-extern from a `.c` import.
