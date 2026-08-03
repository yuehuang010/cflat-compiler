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

## The other half of the same root cause: the QUALIFIED spelling does not resolve at all

Re-measured 2026-08-03 (still reproduces). The flat unqualified map has a second symptom that this
file originally recorded only as a leak - the leak and the miss are one bug:

```cflat
namespace NS { using MyInt = int; }
extern int main() { NS.MyInt x = 5; return 0; }   // unknown type 'NS.MyInt'
extern int main() { MyInt x = 5; return 0; }      // compiles - the leak, prints 5
```

`RegisterTypeAlias` keys on `ctx->Identifier()` verbatim, so the map holds `MyInt`. The unqualified
spelling therefore hits from anywhere (the leak), and the qualified spelling - the one that is
actually CORRECT to write - misses entirely. Fixing the keying closes both at once; a fix that only
scoped the key without teaching lookup the qualified spelling would turn the leak into a false
rejection.

The generic form fails the same way (`list<NS.MyInt>` -> `unknown type 'NS.MyInt'`), which is the
`IsTypeArgTypeKey` follow-on above reached from the other direction.

## Not affected by fix/alias-mangling

The pure-rename mangling fold (`fix/alias-mangling`, 2026-08-02) does NOT touch this.
`PreRegisterRenameAliases` records the alias under the same unqualified spelling, so a namespaced
alias is folded globally by the mangler exactly as `ResolveTypeAlias` already resolved it globally.
That is consistent with today's behaviour and adds no new leak, but it means the namespace-scoping
fix must update `manglingAliases_` keying in the SAME change - it is now a fourth alias map with
the same flat-key defect.

Related: [[interface-issue-queue]] (landed design records)
