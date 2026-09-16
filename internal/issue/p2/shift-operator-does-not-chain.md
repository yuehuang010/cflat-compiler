# Chained '<<' / '>>' does not parse (shiftExpression uses '?' instead of '*')

## Summary

`a << b << c` and `a >> b >> c` are rejected with a parse error. The shift level of the grammar
allows at most ONE shift operator per expression. Plain C/C++ code (bit twiddling, and every
`std::ostream` chain - `std.cout << x << std.endl;`) fails to parse. Found 2026-09-16 while
dogfooding libtorch (scratch/dogfood/torch/probe1.cb); first surfaced as a C++ interop problem
but it is a pure grammar bug with no C++ involved.

## Repro

```c
extern int main()
{
    int a = 1;
    int b = a << 2 << 3;   // error: found '<<' but expected ';'
    int c = 64;
    int d = c >> 1 >> 2;   // error: found '>' but expected ';'
    return b + d;
}
```

```
x64/Release/cflat scratch/dogfood/torch/shift_chain.cb --check
scratch/dogfood/torch/shift_chain.cb(5,19): error: found '<<' but expected ';'
scratch/dogfood/torch/shift_chain.cb(4,20): error: found '>' but expected ';'
```

Workaround: parenthesize - `(a << 2) << 3` parses and evaluates correctly.

## Root cause

cflat/CFlat.g4:158

```
shiftExpression
    : additiveExpression (('<<' | ('>' '>')) additiveExpression)?
    ;
```

The suffix is `?`, so exactly zero or one shift operator is accepted. Every other binary level
uses `*` (`multiplicativeExpression`, `additiveExpression`). `relationalExpression` (line 162)
has the same `?`, but there it is plausibly deliberate (`a < b < c` is a C footgun) - the shift
level has no such excuse and shift is left-associative in C.

## Fix direction

Change the `?` to `*` on `shiftExpression`. The `('>' '>')` two-token spelling exists so the
generic-argument closer `>>` still splits; repeating the group does not change that. Check that
the `MainListener` handler for `shiftExpression` folds N operands left-to-right rather than
assuming at most two. `shiftExpression` is also the compile-time value-argument entry point
(lines 418/448), so generic value arguments gain chained shifts too - verify `A<8 << 1>` still
parses, or keep the value-argument entry at the current single-shift rule if it does not.
Decide separately whether `relationalExpression` stays non-chaining.

Regression test: extend an existing operator fixture (Test/test_operators.cb) with a chained
shift; add a `Test/errors/` row only if relational chaining is ruled to stay an error.

FIXED 2026-09-16 in worktree /Users/felixhuang/source/cflat-shift (branch wip/shift, uncommitted, host-verified 973/0 + LSP): grammar `?` -> `*`, left-fold handler with ParseShiftPair, six rows in Test/test_basic.cb. Delete this file when the branch lands.
