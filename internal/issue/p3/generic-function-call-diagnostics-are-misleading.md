# A failed generic FUNCTION call reports the MANGLED name, invents a candidate, and never mentions arity

Found 2026-07-30 while reviewing layer 4 of [[interface-issue-queue]] (landed design records). **Pre-existing and
identical on the pre-`e2a23d5` binary (the layer-2 tree, since folded into that commit by an amend)** - not caused by that work, and deliberately not fixed there
because it is diagnostic quality, not resolution behaviour.

Three separate defects, all on the same path: when a generic function call fails to resolve, the
error is produced against the MANGLED instantiation name after the call has already been mangled,
so it describes an internal symbol rather than what the user wrote.

## 1. An UNDECLARED generic function prints a PHANTOM candidate

```cflat
namespace D1 { int f() { return nosuchTpl<int>(10); } }
extern int main() { return D1.f(); }
```
```
d1_undeclared.cb(1,32): no overload of 'nosuchTpl__int' matches the given arguments.
  Call arguments (1):
    [0] i8 <unnamed>
  Candidates (1):
    _nosuchTpl__int_nosuchTpl__int__()
```

`nosuchTpl` is declared nowhere. The "candidate" is a zero-parameter function that does not exist -
it is the mangled call name echoed back through the overload printer. The right message is
"unknown generic function 'nosuchTpl'", with no candidate list.

## 2. Wrong TYPE-argument arity says "unknown function", never "arity"

```cflat
namespace D3 { Dt id<Dt>(Dt x) { return x; } int f() { return id<int, float>(1); } }
```
```
d3_typearity.cb(1,62): unknown function 'D3.id__int__float'
```

`D3.id` exists and takes ONE type parameter; the call passes two. The message names a mangled
symbol the user never wrote and does not say the template takes 1 type parameter and got 2.
`gts.genericFunctionTypeParams` holds the declared count at the point of failure, so the message
can state both numbers.

## 3. Wrong VALUE-argument arity leaks the mangled name too

```cflat
namespace D2 { Dt id<Dt>(Dt x) { return x; } int f() { return id<int>(1, 2, 3); } }
```
```
d2_arity.cb(1,62): no overload of 'D2.id__int' matches the given arguments.
  Call arguments (3):
    [0] i8 <unnamed>
    ...
  Candidates (1):
    _D2.id__int_int_int_(int x)
```

This one is the least wrong - the candidate is real - but `D2.id__int` is an internal name and the
user wrote `id<int>`. Worth carrying the spelled name alongside the mangled one.

## Repros

`scratch/l4rev/d1_undeclared.cb`, `d2_arity.cb`, `d3_typearity.cb`. All three behave identically on
the pre-fix binary and on the layer-4 binary.

## Fix direction

The mangled name is built at the call site (`MainListener.h`, the `genericIdentifier` branch of
`PostfixExpression`) before any resolution is attempted, and the failure is then reported by the
generic overload printer downstream. Carry the SPELLED base and the declared type-parameter count
to the failure site and emit a generic-function-specific diagnostic there, instead of letting the
call fall through to the ordinary overload-resolution printer with a mangled name it cannot
explain. Defect 1 additionally needs the "no such template at all" case separated from the
"template exists, no matching overload" case, so it can suppress the candidate list.

Related: [[interface-issue-queue]] (landed design records)
