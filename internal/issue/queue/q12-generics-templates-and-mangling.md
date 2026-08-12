# q12: Generics - templates, type arguments, mangling

6 active items remain. Q12 fixed generic-function registration, generic-interface parent
instantiation, closure type arguments to generic functions, unique array-view rejection, and
variadic free generic functions. Generic structs and generic functions took different paths through registration,
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
- `p2/generic-interface-name-vetoed-by-core-template` - struct/interface tie-break globally favors
  any struct template, even an unrelated core-imported one.
- `p2/last-segment-collision-still-shells-unknown-generic`
- `p3/duplicate-generic-template-name-silently-accepted`

Type arguments:

Mangling / symbols:
- `p2/sizeof-of-generic-instantiation`
- `p2/sizeof-over-generic-instantiation-unresolved-while-alignof-resolves`
- `p2/zero-parameter-generic-function-emits-double-mangled-symbol` (undiagnosed)

Fixed in Q12:

- `p2/generic-function-cannot-be-forward-referenced`
- `p2/generic-interface-cannot-inherit-generic-interface`
- `p2/closure-type-argument-to-a-generic-function`
- `p3/unique-array-view-accepted-as-generic-type-argument`
- `p2/variadic-free-generic-function-does-not-link`

## Fix direction

1. Q12 added the missing whole-TU template collection pass for generic functions in
   `ForwardRefScanner`, mirroring the struct pass. That closes forward references and gives the
   collision check somewhere to live.
2. Q12 routes generic-function type arguments through `ResolveTypeArgEntry`; the closure and array-view
   items then reduce to adding the two exclusions in one place.
3. Q12 handles variadic free-function substitution and arity; the zero-parameter item remains
   active because its current repro now passes on the Q12 pre-fix binary and needs a fresh witness.
4. The collision and last-segment shell items remain deferred by their recorded language/diagnostic
   constraints. The two `sizeof` items are coordinated with Q14's grammar work.

Fully disjoint from the ownership buckets; good parallel work. Sequence the two `sizeof` items with
q14, which owns the `sizeof` grammar ambiguity they sit next to.
