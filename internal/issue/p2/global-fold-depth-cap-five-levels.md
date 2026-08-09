# A global's default-construction fold gives up past a fixed recursion depth

Filed 2026-08-09 by review of the global-default-seeding fix (commit
"Give a global the constant value of its own default construction"). Not a
regression - every cell below reads 0 on the pre-fix binary too - but the fix
turned "always 0" into "0 past a cliff nobody can see", which is worse to debug
because the neighbouring shallower spelling is correct.

Severity: silent wrong value with a located note. The note fires, so it is not
completely silent, but the note does not say WHY and the remedy is not obvious.

## Repro

`FoldConstructedValueToConstant` (MainListener_Declarations.cpp) carries
`depth > 16` as a bail-out. Measured cliffs on the fix binary:

Nesting - `struct D0 { int v = 100; }; struct Dn { D(n-1) inner; int w = n; };`
declared as a global `Dn gdeep;`:

```
D3  global w=3 v=100   local w=3 v=100     folds
D4  global w=4 v=100   local w=4 v=100     folds
D5  global w=5 v=100   local w=5 v=100     folds       <- last folding level
D6  global w=0 v=0     local w=6 v=100     ZEROED + note
D7  global w=0 v=0     local w=7 v=100     ZEROED + note
D8  global w=0 v=0     local w=8 v=100     ZEROED + note
```

Width - a struct whose fields are all `W0` (`struct W0 { int v = 9; };`), plus
one `int t = 5`, declared as a global:

```
13 struct-typed fields   global 9,5   local 9    folds   <- last folding width
14 struct-typed fields   global 0,0   local 9    ZEROED + note
40 struct-typed fields   global 0,0   local 9    ZEROED + note
```

Probes: `scratch/rgs_depth{3..8}.cb`, `scratch/rgs_wq{13,14,...}.cb`.

## Root cause

Two different costs share one budget.

A nesting level costs roughly 2.7 depth units: the field's `call` to its own
constructor (+1), the enclosing `insertvalue` that stores the result (+1), and
the aggregate operand the walk recurses through (+1, amortised). 16 / 2.7 is
five levels, which is exactly where the cliff was measured.

A struct-typed FIELD costs about 1 unit, because the synthesized constructor
builds its result as a LINEAR `insertvalue` chain and the walk recurses on the
aggregate operand - so the Nth field sits N links down. Hence the width cliff
at 13-14.

**Correction to a claim made during review: width is NOT free.** It is free only
for SCALAR/constant fields, and for a measured reason - `IRBuilder::CreateInsertValue`
constant-folds eagerly when both operands are already Constants, so an
all-constant chain collapses to a single `ConstantStruct` and the walk sees no
instructions at all. Measured: a struct with 20 plain `int` field defaults folds
fine as a global (`scratch/rgs_wint20.cb`), while 14 struct-typed fields do not.
Any field that is a real `call` breaks the eager folding from that link onward.

## Fix direction

Do not just raise the constant - the walk has no memoisation, so a wide-and-deep
type is exponential in the shared subtrees, and a bigger budget makes that
reachable rather than fixing it. The shape that would actually work:

1. Memoise per `llvm::Function*` - a no-arg constructor's folded value is a
   property of the function, not of the call site, so one entry per constructor
   makes repeated field types free and turns the walk linear.
2. Then count only real recursion (constructor frames), not `insertvalue` links,
   so nesting and width stop competing for one budget. Walking an
   `insertvalue` chain iteratively rather than recursively removes the width
   cost entirely.
3. Only then raise or drop the cap, with a cycle guard keyed on the function.

Needs its own sweep: raising what folds means more globals change value, and the
accept set has to be re-measured (the original fix measured it empty across
`core/`, `Test/` and `example/`).
