# Negative integer literal in a global (const) initializer is truncated to minimal unsigned width

## Summary

A negative integer literal used as the initializer of a **global** `int`/`const int`
(file scope) is miscompiled: the value is sized to the smallest type that holds its
magnitude and then treated as UNSIGNED, so it reads back as a large positive number.

- `const int A = -150;` reads back as `65386` (0xFF6A, i.e. -150 masked to 16 bits).
- `int B = -101;` reads back as `155` (0x9B, i.e. -101 masked to 8 bits).

The width varies with the magnitude (8-bit for -101, 16-bit for -150), which points at
the literal being classified by minimal-magnitude width, sign dropped.

## Repro

```cflat
const int A = -150;
int C = -150;
extern int main() { printf("%d %d\n", A, C); return 0; }   // prints: 65386 65386
```

Both `const int` and plain global `int` are affected. A LOCAL `int x = -150;` and a
`return -150;` are CORRECT (print -150). Only the global-initializer path is wrong.

## Workaround (in use)

Write the value as a subtraction, which constant-folds correctly:

```cflat
const int A = 0 - 150;   // reads back as -150
```

`example/ui/win32_native_host.cb` declares the ListView LVN_*/NM_* notify codes
(-101/-150/-155/-3) this way with a comment pointing here.

## Root cause (hypothesis, unverified)

The global-initializer constant path likely infers the literal's type from the
magnitude of the token AFTER the unary minus is folded in (or folds `-N` into an
unsigned N of minimal width, then stores the bit pattern zero-extended). A local
initializer / return goes through the normal expression path where the unary minus
produces a correctly sign-extended `i32`.

## Fix direction

In the file-scope initializer evaluation, evaluate a leading unary minus on an integer
literal as a signed i32 (match the local-expression path), or default integer-literal
type to i32 before applying the sign, so a negative global const is sign-extended. Add
a regression test mirroring the repro (global `const int` and plain global `int`).
