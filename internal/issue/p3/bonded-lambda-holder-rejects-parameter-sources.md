# A bonded-capture lambda whose source is a by-value PARAMETER is still rejected when stored in a stack-local holder

Filed 2026-08-22 from the round-3 review of the lambda-capture relaxation. Conservative (a false
REJECT), not a safety hole.

## Repro

```cflat
import "list.cb";
struct Holder { Lambda<int()> f = default; };
int sum(list<int> xs)
{
    Holder h;                       // declared after the source `xs`, same scope -> should be accepted
    h.f = () => { return xs.count(); };
    return h.f();
}
```

Observed: `bonded value cannot be stored in a struct field or through a pointer - bond lifetime
would be untrackable`. Expected: accepted - the parameter outlives every local of the function.

## Root cause

Function arguments are registered by a path that bypasses the central local-registration stamp
(`LLVMBackend_CodegenHelpers.cpp` ~122), so their `NamedVariable::DeclSequence` stays 0, and
`AllowBondedClosureFieldStore` (`MainListener_Expressions.cpp` ~80) rejects any equal-depth source
with sequence 0 rather than treating it as "declared before everything".

## Fix direction

Stamp parameters with a sequence at registration (they precede every local), or treat sequence 0
at the function's outermost depth as older-than-all. Accept-set legs: holder after parameter
(accept); holder in an inner block (accept); the existing reject cells must stay rejected.

## Related note (same review)

`AdoptWrapperProvenance` (`MainListener_Expressions.cpp` ~10713) does not copy
`FieldPathThroughPointer`; no false accept was reproduced through a parenthesized pointer path,
but the field should be propagated with the other provenance bits.
