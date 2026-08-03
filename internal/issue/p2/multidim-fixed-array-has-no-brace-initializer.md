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
