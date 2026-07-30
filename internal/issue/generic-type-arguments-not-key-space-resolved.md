# Generic type ARGUMENTS are not resolved against the namespace key space, so two callers share one instantiation

Filed 2026-07-29 (round 3 of the key-space work). Found by an adversarial review of
[[generic-template-namespace-key-space]], which fixed the template BASE but not the ARGUMENTS.

Severity: **silent wrong value.** Wrong on the pre-fix compiler too, so it is not a regression -
but the key-space fix FLIPPED which caller gets the wrong answer, onto the namespace-local one,
which is the case that work exists to make usable. See "What the base fix changed" below.

## Repro

`scratch/rev/p4_typearg_collide.cb`

```cflat
struct Item { int v = 9; };
struct Box<T> { T t = default; int Get() { return t.v; } };
namespace A {
    struct Item { int v = 7; };
    int f() { Box<Item> b = default; return b.Get(); }   // Item means A.Item -> 7
}
int g() { Box<Item> b = default; return b.Get(); }        // Item means ::Item  -> 9
extern int main() { printf("inA=%d global=%d\n", A.f(), g()); return 0; }
```

Correct answer: `inA=7 global=9`. Actual:

| binary | output |
|---|---|
| pre-fix (`09f1d56`) | `inA=7 global=7` |
| post base fix | `inA=9 global=9` |

Both are wrong, and both give the SAME answer to two callers that must differ.

## Root cause

`MangledGenericName` mangles the type ARGUMENT by its spelled text
(`MainListener.h`, `MangleTypeArg` via `ResolveTypeArgEntry`). `Item` and `A.Item` therefore both
mangle to `Box__Item`, so the two uses collapse onto ONE instantiation. Which `Item` its `t` field
gets is decided by whatever `currentNamespace_` happens to be when that single instantiation is
drained - the first caller to queue it wins, and the second silently borrows its layout.

This is the same defect as the base-name bug that
[[generic-template-namespace-key-space]] fixed, one level down: the base is now resolved through
`LLVMBackend::ResolveGenericTemplateBase`, the arguments are not.

## What the base fix changed

Nothing about the wrongness, only its direction. Pre-fix, `Box<Item>` inside `namespace A` resolved
its ARG outward to the global `Item`, and the global caller queued first, so both got `7` by
accident of drain order. Post-fix the namespace walk reaches `A.Item` for the layout while the
mangled name stays `Box__Item`, and both callers get `9`. The regression risk is unchanged (it was
already broken) but the failure now lands on the namespace-local caller, so this should be fixed
before namespaced generics are advertised as working.

## Fix direction

Resolve type arguments through the same enclosing-namespace walk the base uses, and **carry the
resolved argument into the mangled name** (`Box__A.Item`), so the two uses become two
instantiations. Dots are already legal in mangled names - a namespaced non-generic struct lowers as
`%NS.Plain` today, and the base fix ships `NS.Box__int` - so no new escaping scheme is needed.

Two consequences to plan for:

- **This is a mangled-name change, so it is `--init` relevant** per CLAUDE.md's load-bearing
  serializer rule: cached `instantiated_generics` entries and every cached template's mangled
  references must move in the same change, or a warm cache resolves to the old names.
- The resolution must accept only a candidate that actually names a TYPE (mirroring
  `ResolveGenericTemplateBase`, which accepts only a candidate that names a template), so an
  unrelated namespace sibling never hijacks an argument spelling.

## Test coverage

None. `scratch/rev/p4_typearg_collide.cb` is the repro; `scratch/rev/p4b_typearg_layout.cb` is a
related probe that errors on both binaries (different messages) and should be re-checked once this
is fixed. A fix belongs as a positive leg in `Test/test_generics.cb` next to the `testGnNs*` set.

Related: [[generic-template-namespace-key-space]], [[interface-issue-queue]]
