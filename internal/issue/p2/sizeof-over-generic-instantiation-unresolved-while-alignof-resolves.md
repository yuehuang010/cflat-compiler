# `sizeof(Box<int>)` says "unknown type" while `alignof(Box<int>)` resolves it

Filed 2026-08-06 by `fix/sizeof-closure`, which fixed the SIGSEGV half of this area
(`sizeof-generic-over-closure-type-segfaults-compiler`, deleted) and left this feature gap
open deliberately - the crash fix's floor was a located diagnostic, and this is the diagnostic
being wrong about what the compiler can actually compute.

Severity: located hard error on a spelling the compiler has the answer for. No wrong value.

## Repro - measured on `f24fb18` + the `fix/sizeof-closure` fix, both identical

```cflat
struct Box<T> { T item = default; };
extern int main() { printf("A=%d\n", (int)alignof(Box<double>)); return 0; }   // prints 8
extern int main() { printf("S=%d\n", (int)sizeof(Box<double>));  return 0; }   // unknown type 'Box<double>'
```

`alignof` is not guessing: `alignof(Box<double>)` = 8, `alignof(Box<char>)` = 1,
`alignof(Box<BigA>)` = 32 for `struct BigA { alignas(32) int a = default; };` - three correct
answers off one template. Declaring a `Box<int>` variable first does NOT make `sizeof` resolve.

## Root cause (confirmed by reading the grammar and the two handlers)

`unaryExpression` is `('sizeof')* ( postfixExpression | ... | ('sizeof'|'alignof') '(' typeName ')' | ... )`
(`cflat/CFlat.g4:110`). The leading `('sizeof')*` loop consumes the `sizeof` token, after which
`(Box<double>)` matches the `postfixExpression` alternative - so a `sizeof` whose operand can
parse as a parenthesized expression NEVER reaches the `typeName` alternative. `alignof` has no
prefix loop, so it always lands on `typeName`, gets a real `ParseTypeName` result, and resolves
through the generic-instantiation machinery.

The prefix `sizeof` is then serviced by the text-reconstruction handler in
`ParseUnaryExpression` (`cflat/MainListener.h:17712`), which rebuilds a `TypeAndValue` from the
raw operand TEXT and asks `GetType` for it. `GetType` keys `dataStructures` on the mangled
instantiation name (`Box__double`), not on the source spelling, so every generic spelling misses
and reports `unknown type '...'`. A raw closure spelling misses for the same reason:
`sizeof(T)` inside a generic body with `T` = `function<int(int)>` reports
`unknown type 'function<int(int)>'` while the bare `sizeof(function<int(int)>)` - which takes the
`typeName` alternative - returns 8.

## Fix direction

Make the prefix `sizeof` operand reach the same resolution `alignof` gets, rather than widening
`GetType` to accept source spellings. Two candidates, neither trivially contained (which is why
this was not folded into the crash fix):

- Re-parse the reconstructed operand text through the `typeName` rule and hand the result to
  `ParseTypeName`, so both operators share one resolver.
- Change the grammar so the `('sizeof'|'alignof') '(' typeName ')'` alternative wins over the
  prefix loop for a parenthesized operand. Higher risk: the prefix loop is also what makes
  `sizeof(var)` (a fixed array's storage) and `sizeof(expr)` work at all.

Whichever is taken, the accept-set to freeze first is the operand shapes that work today:
`sizeof(int)`, `sizeof(S)`, `sizeof(S*)`, `sizeof(function<int(int)>)`, `sizeof(Lambda<int(int)>)`,
`sizeof(buf)` on a fixed array, and `alignof` of every one of those.

Take [[sizeof-steals-discarded-tuple-comparison-spelling]] in the SAME change. The character test
that stands in for a type parse cannot tell `Pair<int,float>` from the tuple expression `a<b,c>d`;
routing the operand through the real `typeName` rule is what makes that decidable, and it also
makes an expression fallback safe (the parser decides, rather than a null value reaching
`CreateCast`).

## Test coverage

`Test/errors/err_types.cb` pins the CURRENT behaviour with three `expect_error` legs
(`SizeofBox<function<int(int)>>`, `SizeofBox<Lambda<int(int)>>`, `SizeofPair<int,float>`). Those
legs must be REPLACED by value legs when this is fixed, not merely deleted.
`Test/test_interface.cb::testSizeofClosureTypeSpellings` holds the accept-set legs.

Related: [[interface-issue-queue]]
