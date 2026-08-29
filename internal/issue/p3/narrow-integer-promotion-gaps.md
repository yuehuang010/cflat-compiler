# Narrow integer promotion gaps

## Summary

Several narrow-integer expression paths do not apply C integer promotions before
the operation. These are pre-existing-on-master gaps, not part of the signedness
convention fix. C-exact behaviour is the target.

## Measured repros

The still-open cells below were measured on the master binary and are marked
pre-existing-on-master. The clang values are the independent C oracle values.

- The former inferred-local repro `auto y = (u8)0 | (i8)-1` is consumed as
  `4294967295` on master while clang produces `-1`. (A direct `%d` print of
  the master temporary displays `-1`; the inferred unsigned type is exposed
  when the value is widened or compared.) This auto-binding face is fixed by the
  signed result type synthesis in the signedness branch; the regression now
  asserts `-1`. In an explicit `int` context, the same expression produces
  `-1` on both compilers and is not an open gap.
- pre-existing-on-master: `-(u8)128` produces `128`; clang produces `-128`.
  For the signed case, the open runtime shape is `i8 v = -128; -v`, which
  produces `-128` on cflat and `128` on clang. The folded shape `-(i8)-128`
  produces `128` on both. The path is around `MainListener_Expressions.cpp:7821`.
- pre-existing-on-master: `~(u8)128` produces `127`; clang produces `-129`.
  The path is around `MainListener_Expressions.cpp:7869`.
- pre-existing-on-master: `(false ? (u8)255 : (i8)-1) + (i32)-256` produces
  `-1`; clang produces `-257`. The join is around
  `MainListener_Expressions.cpp:4050`.

## Root cause

These paths retain the narrow storage operation or narrow conditional join
instead of promoting operands to `i32` before the operation. The shared rank
helper now assumes the C promotion rule, but these paths do not yet provide its
promoted operands.

## Fix direction

In a dedicated round, extend promotion to unary narrow operators, narrow mixed
binary operands, and narrow mixed-sign ternary arms before their operations.
Preserve C-exact values and signedness through downstream expressions, including
both folded and runtime paths.
