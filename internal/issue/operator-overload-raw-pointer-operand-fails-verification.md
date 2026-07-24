# User `operator==` with a raw-pointer operand is neither selected nor rejected - dies in module verification

Filed 2026-07-24, found while reviewing the owned-pointer-temp fix. Pre-existing and unrelated to
that work - it involves no `move`, no owning types, and no ownership machinery at all.

## Summary

Declaring a binary operator overload whose left operand is a raw pointer type does not make the
overload selectable, but it also does not produce a diagnostic. The compiler falls through to the
builtin pointer comparison, emits a malformed `icmp` mixing a pointer and a struct, and dies in
LLVM module verification.

## Repro (verified against d33b9cf)

```cflat
struct K { int v = 0; };
struct R { int v = 0; };
bool operator==(R* a, K b) { return a->v == b.v; }
extern int main()
{
    R* r = new R();
    K k = default;
    if (r == k) { printf("eq\n"); }
    return 0;
}
```

Output:
```
Module verification failed:
Both operands to ICmp instruction are not of the same type!
  %5 = icmp eq ptr %3, %K %4

Error: module verification failed.
Compilation failed.
exit=1
```

## Why this matters beyond the crash

CLAUDE.md's standing rule: "When encountering a LLVM assert, after identifying the root cause,
then write an proper error message in the compiler to avoid that case." This is the same class -
a malformed-IR failure surfacing as an internal error with no source location, where the user's
actual mistake (or the compiler's actual limitation) is never named.

Two defensible resolutions, and the choice matters:

1. **Make it work** - allow raw-pointer left operands in operator overload resolution, so
   `TryBinaryOperatorOverload` considers this candidate. This is the better outcome if
   pointer-keyed operators are meant to be expressible.
2. **Reject it cleanly** - if a raw pointer left operand is deliberately unsupported, the
   declaration itself should be a `LogError` at the point of the `operator==` definition
   ("operator overload requires a struct or interface operand; 'R*' is a raw pointer"), so the
   user learns at the declaration rather than at an unrelated use site.

Either way the current behavior - silently declare, silently skip during selection, then emit
invalid IR - is the one option that should not survive.

## Root cause direction

Not investigated in depth. `TryBinaryOperatorOverload` (`MainListener.h`, per CLAUDE.md's
"New binary operator" table row) is where candidate selection happens; the pointer-typed left
operand presumably fails a struct/interface precondition there and the code falls through to the
builtin comparison path without reporting anything. The builtin path then compares a `ptr` against
a loaded struct value.

Note the reviewer found this while checking whether `UnregisterOwnedPtrTemp` in
`ApplyMoveParamTransfer` was reachable: the only candidate path for reaching it was a user-defined
`operator==`/`operator<` with a `move T*` parameter, and this bug is why that path could not be
made live. So fixing this may make that invariant guard reachable - worth re-checking
`LLVMBackend.h:10617` when this is fixed.

## Fix direction

Decide between (1) and (2) above first - that is a language-design call, not a bug-fix detail.
Regression test: if rejected, add an `expect_error` leg to an existing `Test/errors/err_operator*.cb`
(or the nearest existing operator error test); if supported, extend `Test/test_operators.cb` with a
pointer-operand leg.
