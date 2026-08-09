# `using Q = P;` is an unknown type whenever `P` is itself an alias of a POINTER type

Filed 2026-08-09 during the review of `fix/voidcall`. Found while checking that the gate's
`ResolveTypeAlias` (a SINGLE-level lookup) is enough for chained aliases. It is - but only
because a chain whose links are non-pointer types is stored already-resolved, and a chain whose
inner link is a pointer type does not compile at all.

Severity: **P3**. Located diagnostic, rc 1, no binary, nothing silent and nothing miscompiled -
the language simply refuses a declaration it looks like it should accept. Measured identical on
the merge base `75b4275` (`/Users/felixhuang/source/cflat-compiler/x64/Release/cflat`) and on
`fix/voidcall`, so it is pre-existing and unrelated to that fix.

## Repro

Non-pointer chains are fine (`scratch/rev3_c01_alias_chain_int.cb`, rc 0 on both):

```cflat
using A = int;
using B = A;
extern int main() { B v = 5; return v == 5 ? 0 : 1; }
```

A chain through a POINTER alias is not (`scratch/rev3_c02_alias_chain_ptr.cb`):

```cflat
using P = int*;
using Q = P;
extern int main() { Q p = nullptr; return p == nullptr ? 0 : 1; }
```

```
rev3_c02_alias_chain_ptr.cb(3,26): unknown type 'Q'
```

Identical in a generic type argument (`scratch/rev3_c03_alias_chain_ptr_generic.cb`,
`Lambda<Q()>` -> `unknown type 'Q'`), and the same shape for `using VP = void*; using VQ = VP;`
(`scratch/rev3_b03_alias_chain.cb`).

The sibling spelling `using R = P*;` fails in the same place but says so out loud, which is what
pins the root cause below (`scratch/rev3_c05_alias_ptr_of_ptr_alias.cb`, identical on both):

```
rev3_c05_alias_ptr_of_ptr_alias.cb(2,0): using alias 'R' = 'int**': 'int*' is not a known type
```

The ONE-level pointer alias works, which is what isolates the defect to the second link
(`scratch/rev3_c04_alias_ptr_generic_1lvl.cb`, `Lambda<P()>` with `using P = int*;`, rc 0 on
both). So does the one-level `void*` alias in both value and return position
(`scratch/rev3_b01_voidptr_alias.cb`, `rev3_b02_voidptr_alias_return.cb`, `ok=1` on both).

## Root cause

Not the lookup depth - `MainListener_Declarations.cpp:1258` already resolves the RHS one hop at
registration (`target = ResolveGenericBaseAlias(ResolveTypeAlias(target))`), which is exactly
why the non-pointer chain works: `using B = A;` rewrites `target` to `"int"` and the
type test below accepts it.

The failure is the type test itself. At `MainListener_Declarations.cpp:1321` registration is
gated on `IsInterfaceType(target) || GetDataStructure(target).StructType != nullptr ||
IsPrimitiveTypeName(target) || IsWinrtFullName(target)` - all of which take a BARE name. For
`using Q = P;` the one hop rewrites `target` to the decorated string `"int*"`, which is not a
primitive name, not a struct and not an interface, so the branch is skipped. `suffix` is empty
(the `*` came from the alias target, not from this declaration), so the pointer branch at 1341
is skipped too and the declaration degrades to a NAMESPACE alias - after which every use of `Q`
is "unknown type".

## Fix direction

Split the resolved target into base name + decoration before the type test, run the existing
test on the BASE, and fold the decoration into the registered spelling
(`RegisterTypeAlias(alias, base + targetDecoration + suffix)`). That keeps `ResolveTypeAlias`
a single lookup - several call sites now depend on it staying cheap, including the void-ness
gates in `MainListener_PostfixExpression.cpp` and `EmitReturnExpression` - and it fixes
`using R = P*;` at the same time, whose louder message ("'int*' is not a known type") is the
same test failing on the same decorated string.

Do not fix this by turning `ResolveTypeAlias` into a chase; the entries are stored resolved by
design and a chase would need the cycle guard `ResolveManglingAlias`
(`LLVMBackend_Interfaces.cpp:177`) carries for its own 8-step loop.
