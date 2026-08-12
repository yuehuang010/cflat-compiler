# q14: Parser and expression grammar edge cases

9 active items remain. A construct is legal in one syntactic position and silently unavailable, mis-parsed, or a
no-op in another.

## Shared root cause

Three sub-clusters:

- **Postfix on a parenthesized expression.** The postfix parser does not accept certain
  parenthesized forms as a receiver, and `(*p)++` operates on the loaded value rather than the
  restored storage address.
- **`sizeof` vs cast ambiguity.** The cast-target rule is tried first, so `(T[N])` after `sizeof`
  parses as a cast; separately the widened `sizeof` type-name grammar also matches tuple-comparison
  text.
- **Recognized only in declaration position.** `simd<T,N>`, `_ = expr`, closure-argument suffix
  validation, and the constructor discriminator are each implemented at one parse site and absent
  from the others that reach the same construct.

## Members

Postfix / receiver:
- `p2/paren-as-cast-method-call-not-parsed` - a parenthesized `is`/`as` expression is not a
  callable receiver.
- `p2/paren-deref-increment-is-a-silent-no-op` - `(*p)++` writes to a temp.
- `p3/inline-deref-of-container-call-result-has-no-storage` - a call result lives in Primary with
  no alloca, so a deref has nothing to load from.
- `p3/void-call-as-chain-receiver-silently-reuses-original-receiver` - a void result leaves the
  receiver empty, so the chain silently keeps the previous one.

`sizeof` / cast:
- `p3/sizeof-steals-discarded-tuple-comparison-spelling`

Declaration-position-only:
- `p2/simd-type-spelling-unusable-outside-declarations`
- `p2/discard-remedy-underscore-unavailable-in-lambda-expression-body` - `_ = expr` lives in
  statement-position parsing, unreachable from the lambda expression-body path.
- `p3/closure-arg-suffixes-unvalidated-in-signature-position` - array-view/pointer suffix checks
  skipped in the type-argument and signature-component funnels.
- `p3/json-ish-brace-literal-still-typed-string` - `ClassifyBrace` returns Verbatim but
  `HasInterpolation` stays true.

## Fix direction

1. For the declaration-position cluster, move each check into the shared type-name/expression
   funnel rather than adding a fourth copy. Remember: any change to `ParseDeclarationSpecifiers`
   must land in BOTH the `ForwardRefScanner` and `MainListener` copies.
2. The remaining `sizeof`/cast item is the tuple-expression ambiguity; keep its parser distinction
   separate from the now-shared type-name path.
3. The remaining `sizeof`/cast ambiguity is grammar ordering in `CFlat.g4`; use a predicate rather
   than reordering alone, and re-run the whole suite - cast-rule changes have broad blast radius.
4. The postfix cluster needs the parenthesized form to produce addressable storage; solve storage
   once and three of the four follow.

Disjoint from ownership. Note `p2/sizeof-over-generic-instantiation...` is filed in q12 but is the
same grammar area - coordinate.
