# q14: Parser and expression grammar edge cases

2 active items remain, both explicitly DEFERRED. This bucket file stays only because of them.

## Closed 2026-08-12

- `p2/simd-type-spelling-unusable-outside-declarations` - FIXED. `GetType` now decodes the
  `simd<T,N>` spelling itself (one decode point), so the cast target, lambda parameter and tuple
  component cells all work. Cell (d)'s claim was STALE: `simd<T,N>[]` was already rejected with a
  good message. Cell (e) is fixed both ways and the message is simd-specific - the generic
  pointer-to-fixed-array wording steers to `T*`, but `simd<T,N>*` is not a supported type either,
  so decaying is not the fix. The FIELD position was silently ACCEPTING the pointer-to-fixed-array
  because the simd branch tested `declSpec->pointer()` while the trailing `*` is an
  `arrayPtrSuffix`.
- `p3/closure-arg-suffixes-unvalidated-in-signature-position` - FIXED, both cells. The array-view
  suffix is now rejected on a FAT closure generic type argument, and `ResolveSigComponentCodegen`
  consults the fat-closure-pointer guard, so all three sites agree. Thin `function<T>` spellings
  are untouched in both cells.

## Deferred members

- `p3/json-ish-brace-literal-still-typed-string` - DEFERRED BY THE MAINTAINER 2026-08-12
  ("I need to think about this more"). Do not implement a reading without a fresh ruling.
- `p3/sizeof-steals-discarded-tuple-comparison-spelling` - still deferred. Its stated blocker was
  fixed in `bc53456`, but that change did not route the prefix-`sizeof` operand through the real
  `typeName` rule, so the parser still cannot settle the ambiguity. See the file for the
  re-measurement; the wording regressed to the mangled name.
