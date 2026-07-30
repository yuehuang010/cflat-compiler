# A `using` type alias declared inside a namespace leaks into the global scope

Filed 2026-07-30, found by the adversarial review of
[[interface-issue-queue]] (landed design records) (round 2). **Pre-existing on both binaries**, not
caused by and not affected by that fix.

Severity: **name leak / silent shadowing.** No wrong value in the repro, but a namespaced alias can
silently answer for a bare spelling anywhere in the file.

## Repro

`scratch/rev5/c3_alias_control.cb`:

```cflat
struct Thing { int v = 7; };
namespace A
{
    using Item = Thing;      // should be A.Item
}
// ... a bare `Item` at GLOBAL scope resolves to Thing here
```

```
c3 global=7 inA=7
```

on `15809e0` and on the type-argument fix. `global=7` is the leak: the alias was declared inside
`namespace A`, so an unqualified `Item` at file scope should not name anything.

## Root cause direction

`RegisterTypeAlias(alias, target)` stores into a single flat `typeAliases` map keyed by the alias
spelling exactly as written, with no namespace qualification - unlike `dataStructures` /
`interfaceTable` / `functionTable`, which are all keyed qualified. `ScanUsingDeclaration` and
`ParseUsingDeclaration` both take the alias name from `ctx->Identifier()` / `ctx->String()` and pass
it through unmodified. The same applies to `functionTypeAliases` and `genericBaseAliases_`.

The fix is to key all three alias maps under `GetCurrentNamespace()` the way the type registries are,
and resolve an alias spelling through the same innermost-first enclosing-namespace walk the type
registries use. Per the standing lesson in [[interface-issue-queue]] (landed design records), the declaring
namespace must be RECORDED at registration, never re-derived from the dotted key.

## Known follow-on: the generic type-ARGUMENT walk will not follow a scoped alias

`LLVMBackend::IsTypeArgTypeKey` - the accept set for resolving a generic type argument's spelling -
consults `dataStructures`, `interfaceTable` and `gts.scannedTypeNames`. It deliberately does **not**
consult the alias maps, because an alias TARGET is explicit at its declaration site and re-resolving
it at the use site is what produced a silent wrong bind in round 3 of
[[interface-issue-queue]] (landed design records) (`scratch/rev/p11_alias_hijack.cb`).

That is correct today only because aliases are global: a bare alias spelling used as a type argument
inside a namespace resolves through `ResolveTypeAlias` before the walk ever sees it. Once aliases are
namespace-scoped, a namespace-local alias used as a type argument in that namespace will no longer be
found, and `IsTypeArgTypeKey` will need a fourth leg that consults the alias maps **through the same
recorded declaring scope** - not by re-resolving the target. Do that in the same change as the
scoping fix, or the scoping fix turns into a false rejection for generic arguments.

Related: [[interface-issue-queue]] (landed design records)
