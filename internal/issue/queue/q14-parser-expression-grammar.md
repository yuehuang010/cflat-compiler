# q14: Parser and expression grammar edge cases

4 active items remain. A construct is legal in one syntactic position and silently unavailable, mis-parsed, or a
no-op in another.

## Shared root cause

Three sub-clusters:

- **Postfix on a parenthesized expression.** Fixed: parenthesized `is`/`as` receivers retain their
  target type, and `++`/`--` use the restored storage address.
- **`sizeof` vs cast ambiguity.** The cast-target rule is tried first, so `(T[N])` after `sizeof`
  parses as a cast; separately the widened `sizeof` type-name grammar also matches tuple-comparison
  text.
- **Recognized only in declaration position.** `simd<T,N>`, `_ = expr`, closure-argument suffix
  validation, and the constructor discriminator are each implemented at one parse site and absent
  from the others that reach the same construct.

## Members

Postfix / receiver:
- Fixed: parenthesized `is`/`as` receivers, parenthesized dereference increments, inline dereference
  diagnostics, and void-call chain receivers.

`sizeof` / cast:
- `p3/sizeof-steals-discarded-tuple-comparison-spelling`

Declaration-position-only:
- `p2/simd-type-spelling-unusable-outside-declarations`
- Fixed: `_ = expr` is accepted in lambda expression bodies, including parenthesized assignment.
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
