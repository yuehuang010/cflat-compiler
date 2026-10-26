# Named function returning a closure does not bind to a `function<Lambda<...>()>` slot

Found while fixing generic-alias-return-skips-signature-check (T411, 2026-10-22). Pre-existing on master,
by-value and `alias` returns alike. Not an alias-ABI defect.

## Repro
```cflat
Lambda<int(int)> t411Global;
Lambda<int(int)> t411GetGlobal() { return t411Global; }
int main()
{
    int bias = 4;
    t411Global = (int x) => { return x + bias; };
    function<Lambda<int(int)>()> get = t411GetGlobal;   // refused
    Lambda<int(int)> result = get();
    return result(7);
}
```
Diagnostic: `cannot bind function 't411GetGlobal' to function-pointer type 'Lambda<int(int)>()': ... the return
type differs`. The `alias Lambda<int(int)>` getter variant is refused the same way.

## Root cause
`FuncPtrSigOfSymbol` (LLVMBackend_Lookup.cpp ~1199) copies `sym.ReturnType.TypeName`, which for a closure return is
the LLVM struct name `__closure_fat_ptr`. The slot's return is the encoded closure name `Lambda$int$int`. Both
resolve to struct components with different StructKey, so `ComponentsProvablyDiffer` (LLVMBackend_Lookup.cpp ~1117)
reports the return as different (`FuncPtrBindReason`, ~1486). The symbol has lost the closure signature.

## Fix direction
Carry the encoded closure name on the function symbol's return (or recover it from the declared return) so the return
component compares by closure signature. Check the --init cache round-trip for symbol returns in the same change.
Keep the mismatch refusal for genuinely different closure signatures.
