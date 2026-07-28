# A bare interface name resolves to file scope BEFORE the enclosing namespace

Found 2026-07-27 while adding the namespace workaround documented for the core
interface-name collision (finding 1 of `iface-namespace-follow-ups.md`). The workaround
is "declare the interface inside a namespace" - but the resulting namespaced interface
cannot be named bare from inside its own namespace, because the file-scope name wins.

CONFIRMED by running on master `9498562` (Release).

## Repro 1 - the core case (this is what makes the documented workaround awkward)

```cflat
namespace nsCore
{
    interface IHashable { int myhash(); };
    class NsHashable : IHashable { int myhash() { return 99; } };
};

extern int main()
{
    nsCore.NsHashable h;
    IHashable v = h;
    printf("%d\n", v.myhash());
    return 0;
}
```

```
ns_bare_iface.cb(4,50): class 'nsCore.NsHashable' does not implement 'IHashable::hash'
```

The bare `IHashable` in the base clause bound to `core/interfaces.cb`'s `IHashable`
(whose method is `hash`), not to the sibling declared two lines above. The diagnostic is
actively misleading: it names a method the user never wrote, and never mentions that two
different `IHashable` interfaces are in play.

## Repro 2 - NOT core-specific, plain user-vs-user

```cflat
interface IThing { int outer(); };

namespace ns
{
    interface IThing { int inner(); };
    class C : IThing { int inner() { return 7; } };
};

extern int main() { return 0; }
```

```
ns_bare_iface2.cb(6,37): class 'ns.C' does not implement 'IThing::outer'
```

So this is a general name-resolution precedence defect, not an artifact of the core
library being implicitly loaded.

## Root cause

Base-clause / type-position interface resolution checks the exact bare name against
`interfaceTable` and accepts the match BEFORE walking the enclosing-namespace chain.
Because a namespaced interface is registered under its qualified name
(`nsCore.IHashable`, as of `c9acb6c`), the unqualified file-scope entry always matches
first. Inner scope should win over outer scope; today outer wins.

See the enclosing-namespace walk added in `c9acb6c`
(`cflat/MainListener.h:322-354` region) and `ResolveInterfaceName`.

## Impact

- The workaround the compiler itself recommends ("declare it inside a namespace") only
  works if every reference is spelled qualified, including references from inside the
  very namespace that declares it. Nothing tells the user that.
- The failure is a confusing diagnostic rather than a miscompile in both repros above,
  because the two contracts differed. **Not yet established** whether two
  same-named interfaces with COMPATIBLE method sets silently bind to the wrong one -
  that would be a silent miscompile of the same family as `c9acb6c`. Check this first.

## Fix direction

Try the enclosing-namespace-qualified candidate BEFORE the bare file-scope candidate when
resolving an interface name from inside a namespace, so the innermost declaration wins.
Order matters more than the candidate set; the candidates are already computed.

Watch for the hazard recorded in finding 3 of `iface-namespace-follow-ups.md`: a surplus
enclosing-namespace candidate can SET `sawSourceImplementor` in
`InterfaceConversionIsProvablyImpossible` (`cflat/LLVMBackend.h:10069-10080`), so a
reordering here can CREATE an impossibility proof, not merely weaken one.

Current workaround, and what `doc/LANGUAGE.md` now documents: spell it qualified
(`nsCore.IHashable`) even inside `nsCore`. `Test/test_interface.cb`'s
`testNamespacedInterfaceShadowsCoreName` depends on the qualified spelling and will
start exercising the bare path only once this is fixed.
