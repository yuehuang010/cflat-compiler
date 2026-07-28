# Generic interfaces still ignore their enclosing namespace

Filed 2026-07-27. This is the DELIBERATE, declared scope limit of the interface-namespace
fix (`c9acb6c`), not an accident - recording it so it is not lost.

Severity: SILENT MISCOMPILE, same shape as the bug `c9acb6c` fixed for non-generic
interfaces. Two same-named generic interface templates in different namespaces can still
collide.

## What was fixed and what was not

`c9acb6c` made NON-GENERIC interfaces register as `ns.IFace`. Generic interface TEMPLATES
deliberately keep their bare key, because `genericInterfaceTemplates`, `TypeParams`,
`PackIndex`, `MaterializeGenericInterface`, `MangledGenericName` and the `--init` cache
serializer are ALL keyed on the unqualified template name. Qualifying them is a much
larger change than the non-generic fix.

## Verified NOT a broken hybrid

The review specifically hunted for a half-fixed state where a generic interface resolves
under the namespaced key in one place and the bare key in another - that would be worse
than either endpoint. It is not: a generic interface in a namespace, two same-named
generic templates in two namespaces, the same mangled instance `IBox__int` reached from
two namespaces with i32 vs i64 contracts, and a qualified `ga.IBox<int>` use ALL produce
byte-identical results before and after `c9acb6c`, including identical pre-existing
failures. Generic instances bypass the duplicate guard entirely (no `definitionSite`),
matching the old compiler exactly.

So this is a clean no-op, and the collision that remains is the ORIGINAL bug, unchanged.

## Repro direction

Mirror `internal/issue/iface-definition-ignores-namespace.md` repro A using a generic
interface: two namespaces each declaring `interface IV<T>` with different method return
widths, implement each, convert, and observe the wrong-ABI dispatch.

## Fix direction

Thread the namespace through the generic-interface path the way `c9acb6c` did for the
non-generic one: qualify the `genericInterfaceTemplates` / `TypeParams` / `PackIndex`
keys, `MaterializeGenericInterface`, and `MangledGenericName`. **`MangledGenericName` is
the hard part** - it currently produces `IBox__int`, and a dotted namespace in the mangled
symbol needs a decision (`ga.IBox__int` is already emitted as a lookup key today and
fails; see finding 4 of [[iface-namespace-follow-ups]]).

Per CLAUDE.md's load-bearing `--init` rule, any key change here MUST be applied to the
cache round-trip in `LLVMBackend.cpp` in the same change, or it is silently dropped on a
warm cache.

Related: [[iface-namespace-follow-ups]], [[interface-issue-queue]]
