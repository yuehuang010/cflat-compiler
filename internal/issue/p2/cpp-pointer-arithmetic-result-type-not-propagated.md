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
