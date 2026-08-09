# A multi-dimensional fixed array has no working brace initializer

Filed 2026-08-02 while fixing `multidim-array-view-binding-loses-shape`. PRE-EXISTING and
unrelated to that fix: every measurement below is identical on `5a6580c` and on the fix commit.

Severity: a feature gap. Nothing lies to you - all three spellings are rejected - but the
`T[N][M]` type that `fix/mdview`'s diagnostic points people at can only be brought up with
`= default` plus element-by-element assignment.

## Measured, all three spellings, both binaries

Nested braces are a PARSE error:

```cflat
extern int main(){ char[2][8] b = {{'h','i',0},{'y','o',0}}; return 0; }
// error: extraneous input '{' expecting {...}
```

A FLAT brace list counts against the OUTER dimension only:

```cflat
extern int main(){ int[2][3] a = {1,2,3,4,5,6}; return 0; }
// too many initializers for 'int[2]': got 6 elements
```

String-literal elements hit the fixed-array pointer-store reject, one dimension down:

```cflat
extern int main(){ char[2][8] names = {"ab","cd"}; return 0; }
// cannot store a pointer value into fixed-array storage with dimensions [8] -
// a fixed array is not assignable from a pointer or a string literal.
```

The 1-D forms all work: `char[8] b = {'h','i',0};` prints `hi`, and
`char[2][8] b = default;` followed by element assignment prints correctly.

## Correction 2026-08-09: a FOURTH spelling, and it does NOT reject

Added by the fix for `array-value-init-splat-shares-one-owning-seed`, which measured this cell
while enumerating the value-init splat's axes. The headline above - "nothing lies to you, all
three spellings are rejected" - is FALSE for the NAMED value-init list, which silently
miscompiles instead:

```cflat
struct P { int a = 1; int b = 2; };
extern int main(){ P[2][3] p = {b = 5}; printf("%d,%d %d,%d\n", p[0][0].a, p[0][0].b,
                                                               p[1][2].a, p[1][2].b); return 0; }
// rc 0, prints `1,5 -77135616,-77135784` - only the outer dimension is filled
```

Root cause is local to the splat, not to the list-counting above: the POD arm's memcpy loop in
`MainListener_Declarations.cpp` GEPs `{0, i}` against the OUTER array type and counts to
`ConstArraySize` alone, so for `T[N][M]` it writes one element-sized block at the head of each
ROW and leaves the rest of every row undef. Identical on `c7d5978` and on `fix/splatseed`.

The OWNING twin of that same cell was rc 134 with garbage before `fix/splatseed` and is correct
after it: that fix routes an owning element type to `EmitFixedArrayElementWalk` over
`ConstArraySize * ConstInnerDimensions`, a flat element-pointer walk that is dimension-correct
by construction. Deliberately NOT extended to the POD arm in that commit - the memcpy fast path
is frozen there as a byte-identical-IR accept cell - so the two halves of the value-init splat
now disagree on multi-dimensional arrays, and this is the file that records it. Fixing it is the
same one-line total-slot count plus the same walk.

## Why it matters now

`fix/mdview` rejects every unsized multi-dimensional bracket form and its diagnostic says
"size every dimension ('T[N][M]')". That remedy is sound for the TYPE - `char[2][8] b = default;`
compiles and runs - but a reader who reaches for the obvious C initializer next hits one of the
three errors above. The `char[][8] names = {"ab","cd","ef"};` shape that motivated this file was
rc 0 + SIGSEGV before `fix/mdview` (the outer dimension was silently dropped, so `sizeof` was 8);
it is now correctly rejected, and the natural rewrite is not available.

## Fix direction

Two independent pieces, in this order:

1. Accept a FLAT brace list against the total element count of a multi-dimensional array, so
   `int[2][3] a = {1,2,3,4,5,6};` fills the six slots row-major. `EmitPositionalFixedArrayInit`
   (`cflat/MainListener.h`) already walks a flat list; it counts against `ConstArraySize` alone
   and needs to multiply through `ConstInnerDimensions`. The existing "too many initializers"
   wording still applies, against the new total.
2. Only then consider NESTED braces, which need a grammar change (`initializerList` does not
   admit a nested `{`) and is the larger of the two.

The string-literal element case is a sub-case of `char-array-from-string-literal-has-no-spelling`
one dimension down - do not solve it separately here.

## Related

[[char-array-from-string-literal-has-no-spelling]], [[interface-issue-queue]] (the `fix/mdview`
landed design record, which references this file as the remedy's known gap).
