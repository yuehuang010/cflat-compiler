# An interface/struct-member fixed-array return prototype is accepted, not rejected

Filed 2026-08-06 during review of `fix/extern-array`, which closed the free-function fixed-array
by-value return axis for both the definition and the bodyless-prototype registration sites
(`MainListener_Declarations.cpp:1897` and `:2499`).

Severity: P3, diagnostic quality only - NOT a silent-wrong-ABI hazard like the issue that fix
closed.

## Repro

```cflat
interface IBuf { char[8] get(); }
```

Compiles clean (`--check` passes) instead of being rejected the way a free-function or a struct
member method's DEFINITION already is. Same for a struct-member prototype:

```cflat
struct S { char[8] get(); }
```

Also compiles clean.

## Why this is not the same hazard

Neither shape is reachable to a wrong-ABI call in practice:

- Any concrete DEFINITION implementing `IBuf.get()` is already rejected by the existing
  definition-path guard (`MainListener_Declarations.cpp:1897`) - so `IBuf` can never actually be
  implemented, boxed, or called through. The worst outcome is a late, worse-located error at the
  implementor instead of an early one at the contract declaration.
- A struct-member prototype with no body registers no callable symbol; a call site fails with "no
  overload of 'get' matches" rather than reading garbage. Inert, not dangerous.

## Fix direction

Neither path goes through `MainListener::ParseDeclaration` (the site fixed for free-function
prototypes) or `ParseFunctionDefinition` (the site fixed for definitions). They register through
the interface-method-contract path and the struct-member declaration path respectively (both in
`MainListener_Aggregates.cpp` per the `fix/extern-array` reviewer's spot check - not yet traced to
an exact line). Apply the same
`returnType.ArraySize != nullptr || returnType.AliasArraySize > 0` reject (excluding
`IsArrayView`, `IsSimd`, `Pointer`) at whichever registration point each path uses. Since this is
diagnostic quality rather than a correctness hazard, it can be picked up as a normal-priority fix
whenever convenient rather than urgently.
