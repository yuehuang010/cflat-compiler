# Follow-ups from the interface-namespace fix (c9acb6c)

Filed 2026-07-27 from the round-1 opus review of `fix/iface-namespace`. That branch WAS
merged (the core miscompile is fixed and all gates were green), with these findings
knowingly deferred. Everything below was CONFIRMED by running it unless marked otherwise.

## 1. HIGH - UNDECIDED PRODUCT DECISION: a user interface may no longer share a name with any core interface

`cflat/LLVMBackend.h:8934-8946`

```cflat
interface IHashable { int myhash(); };          // no imports at all
class H : IHashable { int myhash() { return 1; } };
extern int main() { H h; IHashable v = h; printf("%d\n", v.myhash()); return 0; }
```

- master before the fix: compiles, prints `1`
- after the fix: `interface 'IHashable' is already defined at interfaces.cb(96,0) - an
  interface name must be unique within its namespace`

`core/interfaces.cb` is loaded implicitly, so this hard-rejects any file-scope user
interface named `IString`, `IList`, `IQueue`, `IMessage`, `IProcess`, `ICanvas`,
`IHashable`, `ILockable`, `IAllocator`, `IEnumerable`, `ICopyable`, `IReflector`,
`IDictionary`, `ITuple`, `IJSON`, `IJSONArray`, `IUiTest`, `ISharedLockable`,
`ICvWaitable`, `IOptimisticLockable`. Several are names a user would plausibly pick.

**The narrowing is asymmetric with the justification used elsewhere in the same diff.**
On the merged compiler, `struct Dup{int a;}; struct Dup{int b;};` in one file still
compiles silently, and a user `class list` shadowing core's `list` compiles silently.
So interfaces are now STRICTER than structs and classes, not equal to them - the opposite
of the argument used to justify narrowing bare-name lookup.

Has ZERO test coverage. Workaround: put the interface in a namespace.

**The user was asked to choose and did not decide** (session ended at a usage limit). The
options put to them were:

1. (Recommended) Fire the guard only when both definitions are user code at the same
   level, so user code may shadow a core interface as it could before. Keeps the guard for
   the genuine user-vs-user collision the issue actually filed.
2. Keep the hard error; improve the message to say the clash is with a CORE library and
   suggest a namespace; document in `doc/LANGUAGE.md`; add test coverage.
3. Fire only within an explicit namespace, never at file scope (weakest - leaves the
   file-scope collision partly reachable).

**Resolve this before doing anything else in this area.**

## 2. MEDIUM - the def-site key is a BASENAME, so the ORIGINAL MISCOMPILE is still reachable

`cflat/MainListener.h:349-354` - `DefinitionSiteText` uses `GetSourceFileName()`, which
returns the basename.

`da/common.cb` line 1 `interface IFoo { int a(); };` and `db/common.cb` line 1
`interface IFoo { i64 b(); };`, both imported: the guard sees the same key
`common.cb(1,0)` for both, does not fire, and the collision proceeds exactly as before the
fix. Shifting `db/common.cb` down two lines makes the guard fire correctly, which proves
the KEY is the hole, not the logic.

Same cause degrades the diagnostic: `common.cb(3,0): interface 'IFoo' is already defined
at common.cb(1,0)` names two different files indistinguishably.

Fix: use the full path in `DefinitionSiteText`. One line.

## 3. MEDIUM-LOW - a stale safety argument that is not sound

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

## 4. LOW-MEDIUM - the grammar widening silently swallows nonsense type arguments

`cflat/CFlat.g4:799-801`, `cflat/MainListener.h:313-320`

```cflat
interface IBase { int b(); };
interface IDer : IBase<int, float, whatever> { int d(); };
```

Before: parse error. After: **compiles and runs correctly** - `BaseSpecifierName` drops
`genericTypeParameters()` entirely and nothing else looks at them on the interface-parent
path. `whatever` is not even a type. Previously-invalid source is now accepted with no
diagnostic. Either honour type arguments on an interface parent or reject them.

## 5. LOW - newly-parseable spellings that fail with misleading messages

- `interface IExt : IBox<int>` (genuinely generic parent) -> `unknown parent interface:
  'IBox'` (type args dropped before lookup). Before: parse error.
- `class A : ga.IBox<int>` -> `unknown interface: 'ga.IBox__int'` (a dotted name fed to
  `MangledGenericName` while templates stay bare-keyed). Before: parse error.

Both still reject, so no accept-set damage - but the widening advertises capability that
does not exist. See [[generic-interface-namespace-scope-limit]].

## 6. LOW - annotation key / template key split for a namespaced generic interface

PLAUSIBLE by inspection only; not reproducible on macOS.

`cflat/MainListener.h:3530` vs `:3541`: `SetTypeAnnotations(name, ...)` uses the
namespace-qualified name, then the generic branch shadows `name` back to `baseName` for
`genericInterfaceTemplates` / `TypeParams` / `PackIndex`. So `[uuid]` on
`namespace ns { interface IFoo<T> ... }` registers under `ns.IFoo` while every template
consumer looks up `IFoo`. Before the fix both used the bare name. Only reachable through
the Windows `[winrt]` / `[uuid]` path.

## 7. LOW - duplicate diagnostic emitted twice

`Test/errors/err_duplicate_interface_name.cb` prints the diagnostic and
`PASS: expected error received` TWICE (the forward scan and the codegen walk both
register), then links. Cosmetic.

Related: [[generic-interface-namespace-scope-limit]], [[interface-issue-queue]]
