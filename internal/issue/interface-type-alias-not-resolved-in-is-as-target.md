# A `using` alias of an interface is rejected as an `is` / `as` target

Filed 2026-07-29 out of round 5 of the adversarial review of
`generic-interface-registered-as-opaque-struct.md`. **Pre-existing**: fails identically on that
issue's before and after binaries, with a value-level consequence.

## Repro

`scratch/rev5/i1_alias_is_target.cb` (read-only review evidence):

```cflat
import "test_helper.cb";
interface IA { int Get(); };
interface IB { int Other(); };
class CA : IA, IB { int d = default; int Get() { return d; } int Other() { return 2; } };
using AliasIA = IA;
using AliasIB = IB;
extern int main()
{
    CA c = default; c.d = 7;
    IA ia = c;
    printf("is-alias-cls: %d\n", ia is CA ? 1 : 0);
    printf("is-alias-iface: %d\n", ia is AliasIB ? 1 : 0);   // <-- rejected
    printf("is-plain-iface: %d\n", ia is IB ? 1 : 0);        // <-- compiles, prints 1
    return 0;
}
```

```
i1_alias_is_target.cb(12,35): 'AliasIB' is not a known struct type for 'is' check
```

`ia is IB` on the very next line compiles and answers `1`. Deleting only the `using AliasIB = IB;`
line and its use makes the file compile (`scratch/rev5/i1b_noalias.cb`), so the alias is the sole
cause. The generic form reproduces too (`scratch/rev5/i2_alias_generic_is_target.cb`).

## Root cause

`LLVMBackend::IsInterfaceType` resolves aliases:

```cpp
bool IsInterfaceType(const std::string& name) const
{
    return interfaceTable.count(ResolveTypeAlias(name)) > 0;
}
```

but every DIRECT `interfaceTable.find/count` site does not. `GenerateIsCheck` uses the direct form
(`MainListener.h`, the `interfaceTable.count(targetTypeName)` interface-target arm), so an aliased
interface name misses and control falls through to the concrete-struct arm, which then reports "not
a known struct type".

## The same asymmetry exists at eleven other sites

Found by a full sweep of every interface-membership decision. `IsInterfaceType` and
`GetInterfaceFields` apply `ResolveTypeAlias`; these do not, so each answers differently from
`IsInterfaceType` for an aliased interface name:

- `HasInterfaceMethod`, `FindInterfaceMethod`, `GetInterfaceMethodReturnType`
- `InterfaceDtorSlotIndex`, `EmitInterfaceFieldAddress` (both silently fall through with
  `methodCount = 0`, yielding a vtable slot index that is too small rather than an error)
- `GetType`'s `isInterface`
- all four interface-target/source arms of `GenerateIsCheck`
- all four of `GenerateSafeCast`

Only the `is`/`as` target arm is confirmed reachable with a user-visible consequence; the others are
plausible but unprobed.

## Fix direction

Resolve the alias once at the top of `GenerateIsCheck` / `GenerateSafeCast`
(`targetTypeName = ResolveTypeAlias(targetTypeName)`) for the immediate bug. The durable fix is to
make `interfaceTable` lookups go through one accessor that always resolves - e.g. a
`FindInterface(name)` returning a pointer - and convert the direct `.find/.count` sites to it, so the
asymmetry cannot be reintroduced.

Add regression legs to `Test/test_interface.cb` covering `is`/`as` with an aliased interface target,
an aliased interface source, and a method call through an aliased interface type.
