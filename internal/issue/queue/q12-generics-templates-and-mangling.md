# q12: Generics - templates, type arguments, mangling

11 items. Generic structs and generic functions took different paths through registration,
type-argument resolution, and name mangling, and the function path is missing pieces the struct
path has.

## Shared root cause

Three sub-themes, all "the generic path is a parallel implementation that drifted":

- **Template registration.** No whole-TU collection pass registers generic FUNCTION templates
  before use scanning (structs have one), template name maps share a key space with no collision
  check, and the shell-acceptance fallback matches on the last dotted segment, ignoring namespace.
- **Type arguments.** Generic-function type-argument resolution bypasses `ResolveTypeArgEntry`,
  which generic structs use, so closures and unique array views are handled wrongly or not at all.
- **Mangling.** `sizeof` resolves its operand as raw text, and the variadic and zero-argument
  paths apply mangling a different number of times on the declaration and the definition, emitting
  unlinkable double-mangled symbols.

## Members

Registration / templates:
- `p2/generic-function-cannot-be-forward-referenced`
- `p2/generic-interface-cannot-inherit-generic-interface` - base clause spelled without type
  arguments, so the lookup misses the mangled instance.
- `p2/generic-interface-name-vetoed-by-core-template` - struct/interface tie-break globally favors
  any struct template, even an unrelated core-imported one.
- `p2/last-segment-collision-still-shells-unknown-generic`
- `p3/duplicate-generic-template-name-silently-accepted`

Type arguments:
- `p2/closure-type-argument-to-a-generic-function`
- `p3/unique-array-view-accepted-as-generic-type-argument` - unique-base rejection omits the
  `hasArrayView` exclusion.

Mangling / symbols:
- `p2/sizeof-of-generic-instantiation`
- `p2/sizeof-over-generic-instantiation-unresolved-while-alignof-resolves`
- `p2/variadic-free-generic-function-does-not-link` (undiagnosed)
- `p2/zero-parameter-generic-function-emits-double-mangled-symbol` (undiagnosed)

## Fix direction

1. Add the missing whole-TU template collection pass for generic functions in
   `ForwardRefScanner`, mirroring the struct pass. That closes forward references and gives the
   collision check somewhere to live.
2. Route generic-function type arguments through `ResolveTypeArgEntry`; the closure and array-view
   items then reduce to adding the two exclusions in one place.
3. Make mangling idempotent, or assert single-application, so the variadic and zero-arg paths
   cannot double-apply. The two undiagnosed link failures should be confirmed against the emitted
   symbol table before writing the fix.
4. Namespace-scope the shell-acceptance key (overlaps q03 - coordinate the key convention).

Fully disjoint from the ownership buckets; good parallel work. Sequence the two `sizeof` items with
q14, which owns the `sizeof` grammar ambiguity they sit next to.
