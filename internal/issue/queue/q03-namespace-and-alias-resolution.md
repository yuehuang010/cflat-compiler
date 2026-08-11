# q03: Namespace and alias resolution

9 items. Type, interface, and alias registries disagree about whether a key is the unqualified
spelling or the namespace-qualified one, and lookups pick whichever convention their author
assumed.

## Shared root cause

Registration and lookup use different key conventions, and the lookup order checks the global
table before walking the enclosing-namespace chain. `typeAliases` is keyed by unqualified spelling
only, unlike the struct/interface registries, so an alias declared inside a namespace is visible
everywhere and an alias of a sibling type is not registered at all.

## Members

- `p2/bare-interface-name-resolves-outward-before-namespace` - global table consulted before the
  enclosing-namespace chain.
- `p2/namespaced-interface-shadowed-by-global-is-broken` - bare-tail vs qualified key mismatch
  (undiagnosed).
- `p2/namespaced-struct-static-method-not-dispatched` - qualified type name unrecognized at static
  dispatch, falls through to variable lookup (undiagnosed).
- `p2/namespaced-using-alias-leaks-globally` - `typeAliases` keyed unqualified.
- `p2/tuple-sugar-in-namespace-does-not-compile` - shell instantiation never drained inside a
  namespace context.
- `p3/namespace-local-using-alias-of-a-sibling-type-is-not-registered` - registration checks the
  raw name against a qualified struct key, never consults the active namespace.
- `p3/interface-lookup-alias-asymmetry-latent` - 32 of 46 `interfaceTable` lookup sites unaudited
  for alias-resolution bypass.
- `p3/iface-namespace-follow-ups` - grab bag deferred from the earlier interface-namespace fix.
- `p3/alias-of-a-pointer-alias-is-an-unknown-type` - alias chain leaves a decorated `"int*"`
  string that fails the bare-name type test.

## Fix direction

1. Pick ONE canonical key (namespace-qualified) and one `LookupType`/`LookupInterface` helper that
   does alias resolution then the namespace walk then the global fallback, in that order.
2. Re-key `typeAliases` to match, and resolve alias chains to a canonical undecorated name plus a
   separate pointer/decoration count.
3. Convert the 46 `interfaceTable` sites to the helper - `p3/interface-lookup-alias-asymmetry-latent`
   IS that audit, so it closes as a side effect.
4. `p3/iface-namespace-follow-ups` is a checklist; split what survives back out as its own file.

Low ownership risk, high issue count - a good parallel work item alongside the q01/q05 chain.
