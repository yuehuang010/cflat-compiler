# A generic interface 'IFace<T>' is registered as an opaque struct and is unusable in most positions

Filed 2026-07-27 by an interface bug hunt against master `dcb9003`.

Severity: LLVM verifier failure reachable from plain source, plus false rejections with
nonsense diagnostics.

## Repro A - verifier failure on a parameter

```cflat
interface Container<T> { T Get(); void Set(T v); };
class Storage<T> : Container<T> { T data = default; T Get() { return data; } void Set(T v) { data = v; } };

extern int main()
{
    Storage<int> s = default;
    s.Set(42);
    Container<int> ci = s;
    printf("boxed arg: %d\n", sumInt(ci));
    return 0;
}
int sumInt(Container<int> c) { return c.Get(); }
```

Exit 1, expected `42`:

```
Module verification failed:
Call parameter type does not match function signature!
  %3 = load %__iface_fat_ptr, ptr %ci, align 8
 %Container__int = type opaque  %4 = call i32 @_sumInt_int_Container__int_(%__iface_fat_ptr %3)
storing unsized types is not allowed
  store %Container__int %c, ptr %c1, align 1
```

Same family:

- Parameter, function declared first: `Unknown identifier 'Get'.` (false rejection).
- `struct H { Container<int> c = default; };`: `Invalid InsertValueInst operands!`
- `list<Container<int>>`: `'Container__int' does not implement interface 'Container__int'`
  (nonsense diagnostic).
- `move Container<int> mk()` return type: same nonsense self-check.
- Forcing the instantiation with a file-scope global before the function does NOT help,
  so this is not an ordering-only issue.

Control: a LOCAL of generic-interface type works, and a generic class implementing a
NON-generic interface works everywhere.

## Root cause

`ForwardRefScanner::ScanGenericTypeUses`, the `tryPreDeclare` lambda at
`cflat/MainListener.h:1912-1920`:

```cpp
auto tryPreDeclare = [&](const std::string& baseName, CFlatParser::GenericTypeParametersContext* genericParams)
{
    ...
    compiler->CreateStructType(mangledName, {});                      // unconditional
    LLVMBackend::TypeAndValue returnType{ .TypeName = mangledName };
    compiler->CreateFunctionDeclaration(mangledName, returnType, {});
};
```

It fires for EVERY `Base<Args>` type specifier in the tree with no check against
`genericInterfaceTemplates`, so `Container__int` is entered into `dataStructures` as an
opaque named struct (`CreateStructType`'s empty-field branch, LLVMBackend.h:13897).
`InstantiateGenericInterface` (MainListener.h:3522-3569) later also enters it in
`interfaceTable`, so the name lives in BOTH maps.

`GetType` (LLVMBackend.h:15676, `bool isInterface = interfaceTable.count(...)`) checks
`interfaceTable` first, which is why a local resolves correctly. But any signature,
field or element type materialized before `ProcessPendingInstantiations` runs the
interface instantiation gets the opaque struct, and the assignment/boxing paths that
consult `dataStructures` produce the `'X' does not implement interface 'X'` self-check.

## Fix direction

In `tryPreDeclare`, skip the `CreateStructType` / `CreateFunctionDeclaration`
pre-declaration when `genericInterfaceTemplates.count(baseName)` - an interface
instantiation has no struct shell and no default ctor. Route it through
`QueueGenericInstantiation`, which already distinguishes the interface case. Then
ensure pending INTERFACE instantiations are drained before function signatures and
struct layouts are materialized, so `interfaceTable` is authoritative at
signature-build time.

As a backstop, `LogError` if a name ever lands in both `dataStructures` and
`interfaceTable` - that state is always a bug and currently fails far downstream.

## Variance

Identical at `-O0`, `-g`, `-O2`.
