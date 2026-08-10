# Array '= default' splat runs field-initializer side effects zero times

## Summary

`S[N] a = default;` on the stack default-constructs elements by folding ONE
construction to a constant and splatting it (TryFoldGlobalDefaultConstruction path in
MainListener_Declarations.cpp, landed with the fixed-array-default-skips-field-initializers
fix). The fold replays only the constructed VALUE, so a field initializer with a side
effect (e.g. `int k = bump();` where `bump()` stores to a global and returns a constant)
folds successfully and its side effect runs 0 times.

Values are correct in every spelling. Only the side-effect COUNT diverges, and the
family already disagrees with itself:

| spelling | bump() calls |
|---|---|
| `S one = default;` (scalar) | 1 |
| `S[2] a;` (bare, non-owning) | 1 |
| `S[2] a = {};` | 1 |
| `S[2] a = default;` | 0 |
| `new S[2]` | 2 (per element) |
| dtor-bearing `D[3] b;` (bare, owning) | 3 (per element) |
| dtor-bearing `D[3] d = default;` | 0 |

Repro corpus: was at scratch/rad_side.cb, rad_side3.cb (arrdef review round, 2026-08-09).

## Root cause

FoldConstructedValueToConstant admits a CallInst whose callee is non-declaration,
single-BB, non-vararg and whose return value folds; it does not screen the callee body
for stores/effects. An allocation never folds (a malloc is a declaration call), which is
what routes owning elements to real per-element seeding - but a pure-store side effect
with a constant return slips through.

## Fix direction

Decide the intended per-element-effect semantics for the whole family first (0 vs 1 vs N
is inconsistent across the seven spellings above). If per-element effects must run, the
fold's admission check needs a side-effect scan over the erased temp function (risk: the
scan must not re-retire the err_iface_field_missing aggregate-constant diagnostic that
the fold preserves). Do not fix by removing the fold.
