# A fixed-array FIELD's own `= { ... }` default brace list is silently discarded

Filed 2026-08-06, found by the coverage matrix of `fix/field-brace` (which fixed the
struct/class/container-typed sibling of this - see the landed record for
`struct-field-default-brace-list-discarded` in `interface-issue-queue.md`).

Severity: silent wrong value.

## Repro

```cflat
struct Outer { int[3] a = { 1, 2, 3 }; int b; };
extern int main(){ Outer o; printf("%d %d %d b=%d\n", o.a[0], o.a[1], o.a[2], o.b); return 0; }
```

-> compiles rc 0, runs rc 0, prints `0 0 0 b=0` (expected `1 2 3 b=0`).

Measured IDENTICAL on `68c78fc` (pre `fix/field-brace`) and on `fix/field-brace` -
that fix deliberately does not touch this shape.

Oracle, verified independently: the same list as a LOCAL declarator works.
`int[3] a = { 1, 2, 3 };` inside `main` prints `1 2 3`.

## Root cause

Same family as the fixed sibling: the five default-constructor emitters read only
`initializer->assignmentExpression()` and `initializer->Default()`, so a brace list
matched neither arm. `fix/field-brace` added a third arm
(`MainListener::ParseFieldDefaultBraceInitializer`) that covers the by-value
struct/class/container case, and deliberately bails on `ConstArraySize > 0`:

```cpp
if (field.Pointer || field.ElemPointer || field.IsArrayView || field.ConstArraySize > 0)
    return nullptr;
```

so an array field falls back to the pre-existing "no initializer" handling, which
zero-fills.

## Fix direction

The struct case works by seeding an alloca with the field type's default and calling
`EmitFieldInitializer` (named field-init) or `TryEmitContainerInitializer`. Neither
applies to a fixed array: the list is POSITIONAL and the value has to be built as an
`[N x T]` aggregate for the `CreateInsertValue` into the container. The existing
positional emitter, `MainListener::EmitPositionalFixedArrayInit`, creates and REGISTERS
an array LOCAL, so it cannot be reused as-is - it needs a slot-taking variant (or the
field path needs its own constant/insertvalue builder). Also enumerate multi-dimensional
(`int[2][2] a = {...}`) and `T[]` view fields before landing.

## Neighbouring cell: a NON-AGGREGATE field type (interface / primitive)

Added 2026-08-06 by `fix/iface-global`'s coverage matrix. `ParseFieldDefaultBraceInitializer`
also bails when the field type is not a registered struct at all
(`auto* fieldType = ...StructType; if (fieldType == nullptr) return nullptr;`), so an
INTERFACE-typed or PRIMITIVE field silently drops its list:

```cflat
interface I { int foo(); };
class S : I { int a = default; int foo() { return a; } };
struct H { I h = { a = 1 }; };
extern int main(){ H h = default; return h.h == nullptr ? 5 : 6; }   // exits 5: h.h is null
struct H2 { int x = { 5 }; };                                        // h2.x reads 0
```

Measured identical (exit 5 / exit 0) on `0a45763` and on `fix/iface-global` - that branch
fixed the DECLARATOR paths (global now rejects like local already did) and deliberately
does not touch the field path. Both scopes of the declarator now REJECT this spelling, so
the field position should get the same rejection, not a value: a brace list naming a
concrete type's field on an interface-typed field is not meaningful, and primitive
brace-value-init is not a language feature. Route it through
`MainListener::LogNonAggregateBraceInitReject` (the helper the two declarator scopes share).

## Neighbouring cell, also out of scope and also silently wrong

A POINTER field with a brace default - `struct Outer { Inner* p = { x = 1 }; };` - reads
`nullptr` on both binaries, silently. The LOCAL declarator REJECTS the same spelling
(`LogPointerBraceInitReject`), so this one probably wants the diagnostic rather than a
value.
