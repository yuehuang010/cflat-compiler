# Follow-ups from the interface-namespace fix (c9acb6c)

Filed 2026-07-27 from the round-1 opus review of `fix/iface-namespace`. That branch WAS
merged (the core miscompile is fixed and all gates were green), with these findings
knowingly deferred. Everything below was CONFIRMED by running it unless marked otherwise.

## 1. RESOLVED - user interface sharing a name with a core interface

The user chose option 2 (keep the hard error, name the core library in the message,
document it, add tests). Shipped as `853cb87`: `coreInterfaceDefs_` set, the
core-specific "collides with the core library interface of the same name" message,
`doc/LANGUAGE.md` coverage, and the guard's first tests.

## 2. MEDIUM-LOW - a stale safety argument that is not sound

`cflat/MainListener.h:322-345` carries the comment that a surplus enclosing-namespace
candidate "can never cause a false rejection". Not sound:
`InterfaceConversionIsProvablyImpossible` (`cflat/LLVMBackend.h:10069-10080`) returns
`sawSourceImplementor`, and a surplus candidate can SET that flag - so a surplus candidate
can CREATE a proof, not merely weaken one.

No user-visible regression today (the pre-fix compiler rejected the same program for the
conflation reason), so this is a wrong ARGUMENT protecting correct-by-luck behaviour.
Correct the comment or scope the surplus.

Also stale and now FALSE: `cflat/LLVMBackend.h:10028-10030` - "A base clause is a bare
Identifier in the grammar and a namespaced interface still registers unqualified, so both
sides are already bare." Both halves stopped being true with this fix.

## 3. LOW-MEDIUM - the grammar widening silently swallows nonsense type arguments

`cflat/CFlat.g4:799-801`, `cflat/MainListener.h:313-320`

```cflat
interface IBase { int b(); };
interface IDer : IBase<int, float, whatever> { int d(); };
```

Before: parse error. After: **compiles and runs correctly** - `BaseSpecifierName` drops
`genericTypeParameters()` entirely and nothing else looks at them on the interface-parent
path. `whatever` is not even a type. Previously-invalid source is now accepted with no
diagnostic. Either honour type arguments on an interface parent or reject them.

## 4. LOW - newly-parseable spellings that fail with misleading messages

- `interface IExt : IBox<int>` (genuinely generic parent) -> `unknown parent interface:
  'IBox'` (type args dropped before lookup). Before: parse error.
- `class A : ga.IBox<int>` -> `unknown interface: 'ga.IBox__int'` (a dotted name fed to
  `MangledGenericName` while templates stay bare-keyed). Before: parse error.

Both still reject, so no accept-set damage - but the widening advertises capability that
does not exist. See [[generic-interface-namespace-scope-limit]].

## 5. LOW - annotation key / template key split for a namespaced generic interface

PLAUSIBLE by inspection only; not reproducible on macOS.

`cflat/MainListener.h:3530` vs `:3541`: `SetTypeAnnotations(name, ...)` uses the
namespace-qualified name, then the generic branch shadows `name` back to `baseName` for
`genericInterfaceTemplates` / `TypeParams` / `PackIndex`. So `[uuid]` on
`namespace ns { interface IFoo<T> ... }` registers under `ns.IFoo` while every template
consumer looks up `IFoo`. Before the fix both used the bare name. Only reachable through
the Windows `[winrt]` / `[uuid]` path.

## 6. LOW - duplicate diagnostic emitted twice

`Test/errors/err_duplicate_interface_name.cb` prints the diagnostic and
`PASS: expected error received` TWICE (the forward scan and the codegen walk both
register), then links. Cosmetic.

Related: [[generic-interface-namespace-scope-limit]], [[interface-issue-queue]]
