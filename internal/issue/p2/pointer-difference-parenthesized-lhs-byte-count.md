# Pointer difference with a parenthesized pointer-arithmetic LHS yields a byte count (silent wrong value)

`(ip + 2) - ip` evaluates to 8 for `int* ip`; `ip + 2 - ip` gives 2 (C: 2 in both). Pre-existing on master
33866560, found by the T24 review (2026-10-03).

## Repro

```cflat
extern int main()
{
    int[4] a = {1, 2, 3, 4};
    int* ip = &a[0];
    long d1 = (ip + 2) - ip;
    long d2 = ip + 2 - ip;
    printf("%ld %ld\n", d1, d2);   // prints "8 2", expected "2 2"
    return d1 == 2 && d2 == 2 ? 0 : 1;
}
```

## Root cause (GUESS)

The parenthesized `ip + 2` result loses its pointee type (same producer gap as
p2/cpp-pointer-arithmetic-result-type-not-propagated), so the subtraction is lowered as integer subtraction of
two addresses instead of a ptrdiff divided by sizeof(int).

## Fix direction

Give the `+`/`-` pointer result its full pointer type (pointee included) where it is produced; the difference
lowering then divides by the element size. Add value legs (int, double, struct element sizes; parenthesized and
not) to an existing pointer-arithmetic test.

## Status 2026-10-05: T36 PARKED after 3 rounds (branch fix/t36-ptrarith bc8c2239, worktree cflat-fix-t36-ptrarith)

The branch fixes both issues (pointer arithmetic keeps its pointee type through CFlat/C++ ranking, ctors,
template deduction and pointer difference; void* arithmetic refused; CFlat overload ranking void* vs typed pointer
as a conversion; chained `1 + p + 1`, T** differences) - Sol round 3 confirmed all of that and no pruning
regression over 152+64 clang-checked cells. One P1 remains and it is NOT pointer arithmetic: an in-place fix for a
master bug (C++ template deduction read `1 + abcer + 1` as a double literal because the identifier contains 'e')
was rebuilt in rounds 2-3 as a TEXT split of the argument at top-level + - * / (LLVMBackend_Overloads.cpp ~303-381),
which regresses 20 expressions master and clang agree on (`1e3+(1+1)`, `1e3+(c?1:2)`, `1.0+x` with float x, ...
-> float instead of double; probes scratch/rev_t36_mechanism/ in the worktree, reviews scratch/briefs/t36_review3.md).
Next step (cheap): revert ONLY the literal-text hunk to master's behaviour, file the 'e'-identifier literal bug as
its own issue (fix = propagate expression source type through typed AST, not text), re-run Sol's mechanism + 152-cell
sets and the gate, then land.
