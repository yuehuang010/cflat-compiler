# `using A = B;` inside a namespace, naming a SIBLING type, is never registered

Filed 2026-08-10 by `fix/aliascast` (found while enumerating the alias spelling axis; unrelated to
ownership). Hard error, not a miscompile - the alias simply does not exist.

## Repro

`scratch/ac_c15_ns_inner_alias.cb`:

```cflat
namespace nsy {
    struct NBox { unique Res* item = nullptr; };
    using NB = NBox;                 // sibling, spelled bare
    void f(NBox p) { NBox o = (NB)p; }
}
```

```
ac_c15_ns_inner_alias.cb(9,34): unknown type 'NB'
```

Identical on the merge base and on `fix/aliascast`. Writing the target QUALIFIED
(`using NB = nsy.NBox;`) works - `scratch/ac_c26_ns_inner_qualified.cb` compiles and is rc 0.

## Root cause

`ForwardRefScanner::ScanUsingDeclaration` (and its authoritative twin `ParseUsingDeclaration`)
register the alias only when the target resolves:

```cpp
if (compiler->IsInterfaceType(target) || compiler->dataStructures.count(target) > 0
    || LLVMBackend::IsPrimitiveTypeName(target) || compiler->IsWinrtFullName(target))
    compiler->RegisterTypeAlias(alias, target + suffix);
```

`target` is the raw spelling `NBox`, but the struct is keyed `nsy.NBox`, so every arm is false and
nothing is registered. The enclosing namespace is available (`NamespaceScope` is active during the
scan) - it is just never consulted.

## Fix direction

Run `target` through `ResolveQualifiedName` before the registration test, the same way
`ParseDeclarationSpecifiers` resolves a type spelling (`MainListener_Declarations.cpp:646`). Apply it
in BOTH copies (`ForwardRefScanner::ScanUsingDeclaration` and `MainListener::ParseUsingDeclaration`)
or the two passes register different alias tables. Watch the alias-of-alias hop just below it, which
already calls `ResolveTypeAlias` on the raw target.
