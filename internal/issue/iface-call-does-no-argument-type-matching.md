# `CallInterfaceMethod` does no argument type-matching: an `int` reaches a closure slot

Filed 2026-07-29 while fixing `iface-thin-function-param-no-lowering`. PRE-EXISTING, identical
on `83caa7f` and on the fix branch.

Severity: SILENT MISCOMPILE, then a runtime crash. An argument whose type does not match the
declared parameter at all is accepted by virtual dispatch and lowered by bit-pattern. The
DIRECT path rejects the same program with a clean overload diagnostic, so this is a
type-safety hole specific to the interface arm.

## Repro

```cflat
import "function.cb";
interface I { int lam(function<int(int)> f); };
class C : I { int lam(function<int(int)> f) { return f(5); } };
extern int main(){ C c; I io = c; printf("r=%d\n", io.lam(7)); return 0; }
```

Compiles clean; exit 138 (SIGBUS) at runtime. The literal is emitted as
`inttoptr (i64 7 to ptr)` into the call's code slot and then called.

The direct analogue is rejected at compile time:

```cflat
class D { int lamD(function<int(int)> f) { return f(5); } };
...  d.lamD(7);
```

```
intarg_direct.cb(3,41): no overload of 'lamD' matches the given arguments.
```

## Root cause

`ResolveInterfaceMethodSlot` picks a slot by ARITY and by the overload scorer, but when only
one slot has the right arity it is taken unconditionally (`byArity[0]`, the documented
"historical first-slot pick"), and `CallInterfaceMethod`'s argument loop then lowers each
argument to whatever the parameter's LLVM type is without ever asking whether the argument's
type is convertible. `CreateOverloadedFunctionCall` has no equivalent hole because a failed
match there produces the "no overload matches" diagnostic.

Note the scorer itself is also permissive for this parameter kind: in
`ComputeOverloadFunction`, a function-pointer parameter accepts an argument whose
`BaseType->isPointerTy()` is true, regardless of what it points to. That clause is what let a
DATA pointer widen into a closure slot until the provenance guard was added in
`LowerByValueArg` (see `closure-param-accepts-data-pointer.md` for the direct-path residue).

## Fix direction

Give the single-arity-candidate path the same type gate the scorer applies, so an
unconvertible argument produces "no method of '<iface>.<name>' matches the given arguments"
instead of a bit-pattern lowering. Expect fallout: the interface arm currently RELIES on being
permissive for generic and closure arguments the scorer cannot rank, so tighten with the
existing accept-set, not with strict equality.
