# An interface-typed global with a brace-list initializer silently discards it

Filed 2026-08-02, found by review while auditing the global-struct-positional-init
family for neighbouring shapes. Same SYMPTOM (silent discard, no diagnostic,
global's brace values lost) but a different root cause than the fixed struct/
union/class case, so kept separate and not fixed here.

Severity: silent wrong value / undiagnosed state (see repro - the concrete
observable effect on `gi` was not further characterized here, only that the
brace list compiles clean with no diagnostic, which is the same class of bug
the fixed issue was about).

## Repro

Measured on the POST binary (state after the global-struct-positional-init fix,
including its bare-brace-spelling round; this construct is NOT caught by that
fix's guard):

```cflat
interface I { int foo(); };
class S : I { int a; int foo() { return a; } };
I gi = { a = 1 };
extern int main(){ return 0; }
```

-> compiles rc 0 (no diagnostic at all - the brace list `{ a = 1 }` names a
field of the CONCRETE type `S`, not of the interface `I`, and is silently
dropped either way).

## Root cause

The fix in this branch guards on `compiler->GetDataStructure(scalarTypeName).StructType
!= nullptr` to decide whether a global's brace-list initializer needs the new
reject. An interface name like `I` is not registered as a `StructData` (interfaces
are a separate table, `interfaceTable`) - `GetDataStructure("I").StructType` is
null - so the guard's precondition is false and this shape falls through
unguarded into the pre-existing (undiagnosed) discard behaviour, same
underlying issue as the original (a global's Constant is built from the type's
default value with no application of the brace list), but for a fat interface
pointer instead of a plain struct/union/class - a different LLVM type shape
(`IsFatInterfaceValue()`, see `MainListener.h` ~line 9121) with its own boxing/
upcast machinery, not necessarily fixable by the same "reject in the same
place" approach without checking what that machinery expects to see.

## Fix direction

Not diagnosed to a specific plan. First determine what `right` actually IS for
this declarator (does `IsFatInterfaceValue()` handling even run for a global
with a brace-list RHS, given the RHS parses as an `initializerList`, not an
`assignmentExpression`, so the interface-boxing branch at `MainListener.h` ~9106
guarded by `assignmentExpression != nullptr` likely never fires either) before
deciding whether to reject (extend the existing guard to interface-typed
declarators too, which would need its own honest message - "field = value"
naming a CONCRETE type's field on an INTERFACE-typed declarator is a third,
distinct shape) or implement something. Re-measure with `--out-lli` to see
what Constant (if any) `gi` actually lands as before assuming it is a simple
zeroinitializer like the struct case.
