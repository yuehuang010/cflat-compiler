# Pointer arithmetic result type reaches C++ spelling only, not overload ranking / constructors

T24 (2026-10-03) made `p + 1` spell as the pointer type in the clang argument list, but the recovered type does
not feed CFlat-side ranking. Pre-existing on master; found by the T24 Opus review.

## Repro

```cflat
// header: void pcv(void*); int pcv(char*);   struct VC { VC(void*); };
char[8] m;
pcv(m + 1);          // CFlat picks pcv(void*); clang picks pcv(char*) - silent wrong overload
VC v = VC(m + 1);    // refused with an empty type list; clang accepts
```

Also: `seq.generate(out, out + 4)` (member-template path) - Test/test_cpp_interop_template.cb:3519 currently
EXPECTS this refusal; flipping that leg needs a maintainer ruling (the leg is the old behaviour, not a rule).

## Fix direction

Type the `+`/`-` pointer result at its producer (full pointer type), so ranking, constructor matching, and the
clang spelling all see `char*`. Shares the producer gap with p2/pointer-difference-parenthesized-lhs-byte-count.

P3 leftovers from the same review: the new void* -> T* refusal at C++ calls has no expect_error leg and its message
prints `ptr*` instead of `void*`.

## Ruling 2026-10-05

Template leg ruled FOLLOW CLANG. `p + 1` keeps its pointee type (int*), so template deduction of
`nil_kind(nilPtr + 1)` deduces int* like clang. The expect_error legs at test_cpp_interop_template.cb ~3519
(`nil_kind(nilPtr + 1)`, and the `nullFlag ? nilPtr : nullptr` one if it now resolves) pinned the old limitation:
the implementer may turn them into positive checks (`!= 2` -> return code). Parked T27 work: worktree
cflat-fix-t27-ptrdiff (8 uncommitted files).

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
