# 'function<R(Args)>' as a generic-interface type argument does not round-trip

Filed 2026-07-29 out of round 2 of the adversarial review of
`generic-interface-registered-as-opaque-struct.md`. Fails on both the pre-fix and post-fix binaries
with a clean, located diagnostic in each case - a **gap, not a regression**, and no verifier failure
or crash in either.

## Repro

`scratch/rev2/d4_func_arg.cb` (read-only review evidence; copy before editing):

```cflat
import "test_helper.cb";
interface C<T> { T Get(); void Set(T v); };
class S<T> : C<T> { T d = default; T Get() { return d; } void Set(T v) { d = v; } };
int useC(C<function<int(int)>> c) { function<int(int)> f = c.Get(); return f(5); }
int dbl(int x) { return x * 2; }
extern int main()
{
    S<function<int(int)>> s = default;
    s.Set(dbl);
    C<function<int(int)>> ci = s;
    printf("d4: %d\n", useC(ci));
    return 0;
}
```

- pre-fix: `d4_func_arg.cb(4,59): Unknown identifier 'Get'.`
- post-fix: `d4_func_arg.cb(4,59): cannot assign a struct value to a pointer variable - use getPtr() or take the address with '&'`

The routing fix got the call as far as resolving `Get` on the interface; the remaining failure is in
the RETURN of a closure-typed `T` out of an interface method.

## Root cause direction

A `function<...>` type argument is encoded to a symbol-safe name for mangling purposes
(`EncodeClosureScanner` / `BuildEncodedClosureName`, producing e.g.
`__fatfn_1_3_int_3_int`), and a closure VALUE is a fat `{ptr,ptr}` pair carried on
`TypeAndValue::IsFunctionPointer` plus the recorded signature - not on `TypeName` alone. When `T` is
substituted into an interface method's return type, the substitution restores the encoded TYPE NAME
but not the `IsFunctionPointer` / `FuncPtrReturnTypeName` / `FuncPtrParams` fields, so the assignment
target sees a plain struct where a closure was expected.

`Test/test_closure.cb` and `internal/language-features.md` cover the "closure through a generic
struct" path, which works; the interface-method path is what is missing.

## Fix direction

When an interface method's return or parameter type resolves through a substitution whose value is an
encoded closure name, re-expand it into the full function-pointer `TypeAndValue` the way
`ParseDeclarationSpecifiers` does for a literal `function<...>` spec and for a
`functionTypeAliases` entry. Both `ParseDeclarationSpecifiers` copies need it (ForwardRefScanner and
MainListener), per the both-copies rule in CLAUDE.md.

Add a regression leg to `Test/test_interface.cb` alongside the generic-interface legs.
