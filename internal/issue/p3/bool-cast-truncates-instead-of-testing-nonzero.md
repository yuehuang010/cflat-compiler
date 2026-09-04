# `(bool)2` is false: the integer-to-bool cast truncates to bit 0

Filed 2026-09-03 while excluding `bool` from the `init_capacity` poison fill.

## Summary

Casting an integer to `bool` emits an LLVM `trunc` to i1, so the result is the low bit of the
value: `(bool)2 == false`, `(bool)3 == true`, `(bool)256 == false`. C, C++ and every other
language with a bool conversion define it as `value != 0`. Holds for both a constant
(`(bool)2`) and a runtime value (`int v = 2; (bool)v`).

## Repro

`scratch/q03u_boolcast2.cb` on the poison-fill branch (same lowering as master 94ec41c): the last
two fields print `0 0` for `(bool)2` and `(bool)v` with `v = 2`; C prints `1 1`.

## Root cause (hypothesis, verify first)

The cast path in `CreateCast` / the explicit-cast handler picks `CreateTrunc` for any narrowing
integer conversion, and i1 is treated as "a 1-bit integer" rather than as bool. Conditions
(`if (v)`) go through `CoerceToBoolCondition` (LLVMBackend_ControlFlowAndFunctions.cpp) which
does compare against zero, so only the EXPLICIT cast and implicit assignments into a `bool`
variable are affected - check `bool b = 2;` too.

## Fix direction

When the destination is `bool` (i1), emit `icmp ne value, 0` (and `fcmp une value, 0.0` for
floats, `icmp ne ptr, null` for pointers) instead of `trunc`. Legs in Test/test_basic.cb next to
the existing bool/cast coverage: `(bool)2`, `(bool)256`, `(bool)v` with `v = 2`, `bool b = 2;`,
`(bool)0.5`, `(bool)(int*)nullptr`. Then `array.cb` can drop the `(T)3 == (T)1` bool test in
favour of a proper predicate if one is added.
