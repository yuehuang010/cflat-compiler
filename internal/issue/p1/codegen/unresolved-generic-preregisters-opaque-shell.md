# An unresolved generic name is pre-registered as an opaque struct shell, suppressing `unknown type`

Filed 2026-08-05, found while fixing a Debug-only LLVM assert (see "Backstop already landed" below).
Pre-existing on `master`. Not specific to interfaces, namespaces, or C interop, despite what the
resulting message says.

## The gap

`ForwardRefScanner::ScanGenericTypeUses` pre-declares an opaque struct shell for every syntactic
`Name<Args>` it finds in a BARE (unqualified) type position, **without checking that `Name` names
a known generic template** (`cflat/MainListener.h:3034-3035` -> `tryPreDeclare` at `:2981`, shell
written at `:2998`):

```cpp
case CFlatParser::RuleTypeSpecifier:
{
    auto* typeSpec = static_cast<CFlatParser::TypeSpecifierContext*>(ruleCtx);
    if (typeSpec->genericIdentifier() != nullptr && ... )
        tryPreDeclare(typeSpec->genericIdentifier()->Identifier()->getText(), ...);   // <- NO gate
    // The qualified spelling 'NS.Box<int>', only when it names a CFlat template key:
    // an imported winmd generic is built elsewhere and must get no opaque shell.
    if (std::string qBase; auto* qParams = GenericSpecOf(typeSpec, qBase))
        if (typeSpec->qualifiedGenericIdentifier() != nullptr
            && Compiler(typeSpec)->IsGenericTemplateKey(qBase))                       // <- gated
            tryPreDeclare(qBase, qParams);
}
```

The QUALIFIED path is gated on `IsGenericTemplateKey`; the BARE path immediately above it is not.
The asymmetry looks accidental - the gate's comment justifies it for imported winmd generics, and
says nothing about unknown names.

`ResolveGenericTemplateBase` (`cflat/LLVMBackend.h:10308`) cannot help the caller notice: on a miss
it returns the spelling unchanged rather than signalling failure, so `tryPreDeclare` cannot tell
"real global-scope template named `S`" from "no such name anywhere". It mangles and calls
`CreateStructType(mangledName, {})`, which lands in the opaque branch at
`cflat/LLVMBackend.h:16038` and creates a permanently unsized `llvm::StructType` in
`dataStructures`. Nothing ever calls `setBody` on it, because nothing queued an instantiation.

Verified directly rather than by reading - lldb breakpoint on `LLVMBackend.h:16038` for
`int use(ZZZ<int> v)`, exactly one hit:

```
frame #0: LLVMBackend::CreateStructType(name="ZZZ__i32", typeAndValues=size=0) at LLVMBackend.h:16038
frame #1: ForwardRefScanner::ScanGenericTypeUses(...)::'lambda'(spelledBase="ZZZ", ...) at MainListener.h:2998
frame #2: ForwardRefScanner::ScanGenericTypeUses(...) at MainListener.h:3035
```

The main codegen pass does NOT have this hole - `QueueGenericInstantiation`
(`cflat/MainListener.h:3894`) performs exactly the missing check and no-ops on unknown base names,
and `ParseDeclarationSpecifiers` gates on `isKnownTemplate`. This is a both-copies divergence of
the kind CLAUDE.md warns about, with the ForwardRefScanner copy being the lax one.

## Why it matters: the shell defeats an existing, correct diagnostic

A non-generic unknown type is rejected properly, because `GetType` finds no `dataStructures` entry
and reports at `cflat/LLVMBackend.h:17969`. The shell makes the generic spelling skip that path
entirely. Same file, same position, two different outcomes:

| Repro | Result |
|---|---|
| `ZZZ z;` (non-generic, undeclared) | `unknown type 'ZZZ'` - correct |
| `ZZZ<int> z;` (generic, undeclared) | `type 'ZZZ__i32' has an incomplete layout (a field type C interop could not import)` |
| `int use(ZZZ<int> v) { return 1; }` never called | **compiles, links, and runs clean - exit 0** |

The third row is the serious one: a function signature naming a type that does not exist anywhere
is silently accepted. A typo'd or missing-import generic in a signature raises nothing until
someone materialises a value of it, and then blames C interop.

## Downstream symptoms this explains

The unsized shell is inert until it reaches a by-value use, so where it surfaces depends on where
it lands:

| Landing site | Symptom |
|---|---|
| local variable (`CreateLocalVariable`, `LLVMBackend.h:15170`) | the misleading "incomplete layout / C interop" message |
| by-value parameter (`createFunctionBlock`) | LLVM assert `Cannot getTypeInfo() on a type that is unsized!` on a Debug binary; on Release, silent acceptance when the parameter is unused |

This is the CAUSE of two of the three causes catalogued in
[[incomplete-layout-message-blames-c-interop]] - that P2 correctly observes the message is
structural and says nothing about provenance. Fixing this issue removes most of that P2's reach;
the two should be looked at together.

Note `int use(Box<int> b)` written ABOVE `class Box<T>` also lands here, but that is NOT evidence
of a broken forward reference - the non-generic analog (`int use(S s)` above `class S`) is rejected
too, with `unknown type 'S'`. Use-before-declaration at file scope is simply not supported for
by-value struct parameters; only the MESSAGE differs between the generic and non-generic spellings.

## Backstop already landed (do not mistake it for the fix)

`createFunctionBlock` now gives an unsized shell a sized placeholder slot and skips the store, so
the by-value parameter path no longer asserts inside LLVM. `BaseType` deliberately stays the shell
so member lookup still fails and the accurate downstream diagnostic still fires. That is a crash
backstop only - it does not stop the bogus shell from being created, and the silent-acceptance row
above survives it.

## Fix direction

Gate the bare `tryPreDeclare` call on the same check the qualified call one line below already
uses (`IsGenericTemplateKey`, `cflat/LLVMBackend.h:10291`), so an unresolved base creates no shell
and falls through to the existing `unknown type '...'` diagnostic.

The ordering supports this - the whole-TU template collection runs BEFORE any use is scanned
(`cflat/LLVMBackend.cpp:788-790`):

```cpp
scanner.PreRegisterRenameAliases(tu);
scanner.ScanGenericInterfaceTemplateNames(tu);          // collects EVERY template in the TU first
for (auto* decl : tu->externalDeclaration())
    scanner.ScanGenericTypeUses(decl);                  // only then pre-declares shells
```

`CollectGenericTemplateDecls` records struct/class bare names into `scannedGenericStructNames` and
interface names into `scannedGenericInterfaceNames` (`cflat/MainListener.h:2830`), and imports are
fully compiled before the importing file is scanned, so a legitimately-declared template is always
known by the time its use is scanned regardless of declaration order.

Two traps for whoever takes this:

1. **`certain=false` regions are NOT collected.** Per the comment at `cflat/MainListener.h:2832`,
   `CollectGenericTemplateDecls` skips the struct half inside an unfoldable `if const` arm and
   inside an `expect_error` block. A template declared in either region is absent from the registry,
   so a naive gate would FALSE-REJECT its uses. Check `Test/errors/` and the `if const` tests
   specifically - this is the most likely way a fix goes wrong.
2. **The message will change for existing tests.** The three
   `Test/errors/err_namespaced_generic_iface_*.cb` currently pin `Unknown identifier 'Width'.` /
   `'Tag'.`, which is the CURRENT and deliberately-preserved behaviour (the maintainer confirmed
   that message is correct and useful on 2026-08-05). Gating would move them to an unknown-type
   message. Decide whether that is wanted BEFORE editing, not from a red suite.

Also worth deciding in the same change: `tryPreDeclare` has the source spelling (`spelledBase`, and
the full text via the parser context), so a diagnostic raised there could say `unknown type
'ZZZ<int>'`. By the time the shell surfaces downstream only the mangled `ZZZ__i32` is left -
`TypeAndValue` carries no original-spelling field.

`cflat/LLVMBackend.h` and `cflat/MainListener.h` are both contended - coordinate before editing.

## Related

[[incomplete-layout-message-blames-c-interop]], [[interface-issue-queue]]
