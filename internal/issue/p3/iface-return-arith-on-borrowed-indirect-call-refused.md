# Pointer arithmetic on a borrowed indirect-call result is refused as an interface return

From T81 review 5 (2026-10-10). T81 accepts `return t + 1;` into an interface when the root of `t` is a
borrow, but an INDIRECT call root (interface dispatch, function pointer) refuses, because the callee's
ReturnsOwned fact is unknown at that point (PointerValueDerivesFromOwner, MainListener_Expressions.cpp
~1243). Refused, while plain `return X;` is accepted and safe:
- `IHolder i = &gh; return i.peek() + 1;` (non-move `Thing* peek()`), and `Thing* b = i.peek(); return b + 1;`
- `function<Thing*()> gfp = getB; return gfp() + 1;`, `IThing g(function<Thing*()> fp) { return fp() + 1; }`
The refusal text is also misleading: it says the heap object "will leak" and suggests `move IThing`,
which is then refused too ("returned expression is not owned").

## Repro
scratch/repro_keep/t81/e1.cb, e5.cb

## Fix direction
Record owned call results by value where lastCallReturnsOwned is set (Overloads ~7288 plus the interface
dispatch and function-pointer call sites) and query that record in PointerValueDerivesFromOwner instead of
refusing every indirect call. Until then, at minimum give the indirect-call refusal its own text ("bind the
result to a local variable of the class type first").
