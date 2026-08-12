# q12: Generics - templates, type arguments, mangling

4 active items remain. Q12 fixed generic-function registration, generic-interface parent
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
- **Mangling.** The variadic and zero-argument paths apply mangling a different number of times on
  the declaration and the definition, emitting unlinkable double-mangled symbols.

## RULING 2026-08-11 (maintainer): reject the collision; the test may be renamed

The stated blocker is lifted. `Test/test_generics.cb` may be edited: rename the INTERFACE leg
(`interface Container<T>` at line 244, and its uses through `Storage<T>` and `ExtGet<T>`) to a
distinct name such as `IContainer<T>`, leaving the `struct Container<T>` at line 52 alone.

With that done, option 1 is authorized for BOTH collision items: a generic interface and a generic
struct/class template that share a name and a scope are a hard `LogError` at the SECOND
declaration, mirroring the non-generic guard in `CreateInterfaceDefinition`. That also retires the
core-template veto - `list`, `Pair`, `span`, `queue`, `stack`, `view` and the rest stop silently
vetoing a user's generic interface, because the collision is now diagnosed instead of resolved by
an undocumented struct-always-wins precedence.

Do NOT implement option 2 (a role qualifier at the use site): collisions are not meant to be legal,
so there is nothing to disambiguate.

## Members

Registration / templates:
- `p2/generic-interface-name-vetoed-by-core-template` - struct/interface tie-break globally favors
  any struct template, even an unrelated core-imported one.
- `p2/last-segment-collision-still-shells-unknown-generic`
- `p3/duplicate-generic-template-name-silently-accepted`

Type arguments:

Mangling / symbols:
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
   constraints. Generic `sizeof` resolution is now shared with Q14's type-name grammar path.

Fully disjoint from the ownership buckets; good parallel work. The generic `sizeof` items landed
with q14, which owns the surrounding grammar ambiguity.
