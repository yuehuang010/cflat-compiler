# Direct interfaceTable lookups still bypass alias resolution

Follow-up from `internal/issue/p1/interface-type-alias-not-resolved-in-is-as-target.md`
(fixed: `GenerateIsCheck` / `GenerateSafeCast` in `cflat/MainListener.h` now resolve both
`targetTypeName` and `srcTypeName` through `ResolveTypeAlias` once at the top of each
function, before any `interfaceTable.find/count`; the original spelling is kept separately
for diagnostic text).

## Scope correction: p1's "sweep" covered 12 of 46 sites

`grep -n 'interfaceTable\.\(find\|count\)' cflat/MainListener.h cflat/LLVMBackend.h` finds 46
direct call sites. The p1 issue enumerated twelve of them as "the same asymmetry" and treated
that as exhaustive. It was not - see "Two reachable sites the p1 sweep missed" below. That
leaves 32 sites neither in the original twelve nor in the two newly found reachable ones;
they have NOT been individually probed. Eleven of them are listed at the end of this file as
specific candidates flagged for the next pass - the remaining ~21 have not even been
individually identified yet. Do not assume any of the 32 are safe just because they are not
in the reachable list below; "not yet probed" is not the same claim as "unreachable".

## What was probed and closed (the twelve p1 named)

Every one of the twelve sites named in the p1 issue was given a real `.cb` repro exercising
it with an aliased interface name, run against master (pre-fix) and the fixed binary:

| Site | Verdict | Why |
|------|---------|-----|
| `GenerateIsCheck` - all 4 arms (`MainListener.h:13501`) | REACHABLE | Fixed. |
| `GenerateSafeCast` - all 4 arms (`MainListener.h:13625`) | REACHABLE | Fixed. |
| `HasInterfaceMethod` (`LLVMBackend.h:9576`) | UNREACHABLE | Every caller passes `NamedVariable.TypeAndValue.TypeName`, which both `ParseDeclarationSpecifiers` copies (`MainListener.h`) resolve via `ResolveTypeAlias` at declaration time, before `CreateLocalVariable` ever stores it. |
| `FindInterfaceMethod` (`LLVMBackend.h:9590`) | UNREACHABLE | Only reached through `GetInterfaceMethodParams`, called with the same pre-resolved `TypeName`. |
| `GetInterfaceMethodReturnType` (`LLVMBackend.h:9621`) | UNREACHABLE | Dead code - no callers anywhere in the tree. |
| `InterfaceDtorSlotIndex` (`LLVMBackend.h:12329`) | UNREACHABLE | Only called from `DeleteInterfaceValue`, itself only called with a pre-resolved `TypeName`. Verified at runtime with a 3-method/2-field interface used entirely through an alias, comparing WHICH function ran (not just a count) - byte-identical on both binaries. |
| `EmitInterfaceFieldAddress` (`LLVMBackend.h:12340`) | UNREACHABLE | Same - `ifaceName` argument is always `interfaceVar.TypeAndValue.TypeName`, pre-resolved. Verified at runtime with the same multi-method/multi-field probe. |
| `GetType`'s `isInterface` (`LLVMBackend.h:16658`) | UNREACHABLE, but see the correction below - this is copy-paste duplication, not a reassuring "second resolution". |

Repros live under `scratch/ia_*.cb` in the parent checkout (not committed - throwaway
verification files). `Test/test_interface.cb::testInterfaceAliasIsAsTarget` keeps the
regression coverage for the sites that are ACTUALLY reachable and fixed: all four `is`/`as`
target arms, the alias-of-alias chain, and an alias of a concrete class. Its
`iface_alias_is_source` / `iface_alias_as_source` legs are a TRIPWIRE, not regression
coverage - they pass identically on the pre-fix binary too, because `AliasIPress src = p2;`
is a declaration, so the alias is already resolved by `ParseDeclarationSpecifiers` before
`GenerateIsCheck`/`GenerateSafeCast` ever run. They are kept so a future change that lets an
unresolved alias reach `srcTypeNameIn` would fail a test instead of shipping silently.

## Correction: `GetType`'s alias handling is NOT independent - it is duplication

The original version of this file described `GetType`'s inline alias handling
(`LLVMBackend.h:16616-16640`, `typeAliases.find(resolvedTypeName)`) as "a second,
independent resolution, not the same asymmetry" and treated that as reassurance. That framing
is wrong: it is the SAME `typeAliases` map, the SAME single-hop resolution as
`ResolveTypeAlias`, just open-coded again with extra star/bracket peeling for a pointer or
array alias (and it also resolves enum backing types first, and falls back to
`ResolveQualifiedName` - so it is strictly MORE resolving, not a separate mechanism).
Two independent copies of "look up `typeAliases` once" is exactly the kind of drift that
let the p1 bug happen - this one just happens to currently agree with `ResolveTypeAlias`.
It should be folded into the proposed `FindInterface`/alias accessor below, not left as a
second hand-rolled copy that could quietly diverge from it.

## Two reachable sites the p1 sweep missed: interface `switch`/type-case

Neither of these was in the original twelve, and both are reachable RIGHT NOW, identically
on master and on this branch (pre-existing, not a regression, not fixed by this change):

```cflat
interface IB { int f(); };
class CB : IB { int f() { return 1; } };
using AliasIB = IB;
extern int main()
{
    IB v = new CB();
    int hit = -1;
    switch (v)
    {
        case AliasIB: hit = 1; break;   // <-- rejected
        default: hit = 0; break;
    }
    printf("hit=%d\n", hit);
    return 0;
}
```

Both binaries: `Undefined variable AliasIB.` (`case IB:` with no alias works on both). Root
cause: `MainListener.h:5724`, `exprText = constExpr->getText()` - the raw source text of the
case label is used directly as a name with no `ResolveTypeAlias` call at all (not even the
resolved-vs-original mixup from the p1 sites - this one never resolves either way). The
arm-style spelling (`case TypeName* v => ...`) has the same defect one function up, at
`MainListener.h:5704` (`compiler->interfaceTable.count(typeName)` on `labeled->typeSpecifier()
->getText()`); a repro there (`case AliasCA* v => ...`) fails identically on both binaries
with `'AliasCA' is not a known struct or interface type`.

Not fixed here: out of scope for the `is`/`as` fix on `fix/iface-alias`, and not a regression
introduced by it (confirmed identical on the pre-fix binary). Left for whoever picks up the
`FindInterface` accessor below - the switch/case path needs the SAME treatment as
`GenerateIsCheck`/`GenerateSafeCast`: resolve for lookup, keep the original spelling for
"not a known type"-style messages.

## The 32 sites not yet individually examined

`grep -n 'interfaceTable\.\(find\|count\)' cflat/MainListener.h cflat/LLVMBackend.h` lists 46
direct-lookup call sites in total. Twelve were probed above (ten unreachable + the two fixed
functions, each now 4 call sites internally); the switch/case pair above accounts for two
more REACHABLE ones. The remaining 32 sites have NOT been given a repro and their
reachability is unknown - do not treat silence as a clean bill of health. Eleven were flagged
as specific candidates for the next pass (line numbers as of this commit; re-`grep` before
trusting them after further edits to either file):

`MainListener.h:7398`, `:7639`, `:20813`, `:20872`, `:24977`;
`LLVMBackend.h:9476`, `:10226`, `:11055`, `:11349`, `:13440`, `:13455`.

The other ~21 of the 32 have not even been individually located yet - the 46-site grep count
above is the only accounting of them that exists.

## Why this file exists

Sites confirmed unreachable are unreachable ONLY because `TypeAndValue.TypeName` happens to
always be pre-resolved by the time it reaches them - an invariant of the current declaration
path, not something these functions enforce themselves. `HasInterfaceMethod`,
`FindInterfaceMethod`, `InterfaceDtorSlotIndex`, and `EmitInterfaceFieldAddress` still do a
bare `interfaceTable.find(ifaceName)` with no defensive `ResolveTypeAlias` call. A future
declaration path that builds a `TypeAndValue`/`NamedVariable` without going through
`ParseDeclarationSpecifiers` (both copies) would silently reintroduce exactly the p1 bug -
and for the dtor-slot/field-address pair, as a wrong-but-non-crashing vtable index rather
than a compile error. And the switch/case sites above are a LIVE, if narrow, instance of the
same family of bug today.

## Fix direction (deferred, not urgent)

Per the p1 issue's own "durable fix" note: add one accessor - e.g. `FindInterface(name)`
returning a pointer, resolving the alias internally, folding in `GetType`'s open-coded
`typeAliases.find` too - and convert `HasInterfaceMethod`/`FindInterfaceMethod`/
`InterfaceDtorSlotIndex`/`EmitInterfaceFieldAddress`/the switch-case sites
(`MainListener.h:5704`, `:5724`) to use it, so the asymmetry cannot be reintroduced by a
future caller. The 32 unexamined sites should be swept with the same repro-per-site
discipline before anyone calls the accessor migration complete.
