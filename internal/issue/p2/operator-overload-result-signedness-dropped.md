# Binary-operator handlers drop or improvise the result's signedness in their overload branches

Filed 2026-08-15 by the code review of the u64 shift-signedness fix. That fix corrected the
PRIMITIVE shift branch (`ParseShiftExpression` now returns `lv.isUnsigned`); this records the
neighbouring branches that still get signedness wrong. Pre-existing, not introduced by the fix.

Severity: silent wrong value (probe-proven). No crash, no diagnostic.

## Probe-proven cell - FIXED 2026-08-15

`cflat/MainListener_Expressions.cpp:5396` - the shift OPERATOR-OVERLOAD branch returned
`{ res, false }` even when the overload's return type is unsigned. Now returns
`resultNV.TypeAndValue.IsUnsignedInteger() != -1` (the overload's return type); the probe
flipped from `LE (signed bug)` to the correct unsigned `GT`. The cells below remain OPEN.
Original measurement: a struct
`operator>>` returning `u64` `0x8000000000000000`, its result used directly:

- `result > <literal>` emits `icmp sgt` and reports LE (signed compare of a value above 2^63);
- `result / 2` emits `sdiv` and prints 13835058055282163712 (correct unsigned: 4611686018427387904).

Masked when the result is first bound to a declared `u64` local or the other operand is
explicitly unsigned. `resultNV.TypeAndValue = compiler->lastCallReturnType` is available right
above the return and carries the real signedness.

## Overload branches - ALSO FIXED 2026-08-15

The additive (~5516) and multiplicative (~6001) overload branches used to keep the OPERANDS'
`lu || ru` flag for an overload result; both now take `lastCallReturnType.IsUnsignedInteger()`
when an overload dispatched, matching the shift fix. Regression legs live in
`Test/test_operators.cb` (`UBits`, three legs) - all three fail on the pre-fix binary. The
equality/relational handlers correctly return bool and the bitwise handlers have no overload
dispatch, so every overload branch is now covered.

## What remains OPEN: the primitive-branch convention split

Adjacent handlers answer "what is a PRIMITIVE result's signedness" two different ways:

- bitwise `|` `^` `&` (`MainListener_Expressions.cpp:4228/4250/4272`): `lv.isUnsigned` - LHS only;
- additive and multiplicative: `lu || ru`.

So `signedVal & unsignedVal` reports signed while `signedVal + unsignedVal` reports unsigned for
the same operand pair.

## Fix direction for the remainder

Pick ONE convention - `lu || ru` matches C's value-preserving promotion closest - and apply it
to add/mul/bitwise alike, ideally via one shared helper. This changes observable semantics for
mixed-sign operands, so build the accept-set first: sweep `core/`, `Test/`, `example/` for
mixed-sign bitwise expressions whose downstream ops are sign-sensitive, and add value legs for
both a folded and a runtime operand (the shift fix's legs in `Test/test_basic.cb` are the
template). Needs a maintainer nod on the convention before landing.
