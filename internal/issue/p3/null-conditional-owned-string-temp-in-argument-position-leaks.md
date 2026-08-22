# Null-conditional owned-string temp in argument position leaks

Filed 2026-08-21 while verifying the ?. owned-string-temp fix in the p2 bundle
(commit 3e6162a). Reproduces on **master** too - this is not a regression from
that fix, just an adjacent gap noticed while testing it.

## Repro

```cflat
struct Person
{
    string name = default;
    string label() { return this->name + "!"; }
}

int takeLen(int n) { return n; }

int main()
{
    Person? p = default;
    int n = takeLen(p?.label());
    return n;
}
```

Compiled and run with `--atExit` leak checking, this leaks 16 bytes - one
owned-string allocation's worth (stringbuilder/string header + short-string
or heap payload).

## Root cause guess

The p2 fix (see commit 3e6162a) made the `?.`-chain's owned-string temporary
get destructed correctly when it is the *whole expression* (e.g. an
assignment RHS or a bare statement). When the `?.` result instead lands in
**argument position** of a call (`takeLen(p?.label())`), the temporary's
lifetime is scoped by the call-argument machinery, which does not know about
the null-conditional temp-cleanup path introduced by that fix - the
owned-string destructor call is only threaded through the "expression
statement / assignment" cleanup point, not the "call argument" one.

Likely lives in `MainListener_Expressions.cpp` (or wherever `?.` chains
lower to a null-check branch producing a `TypeAndValue`) alongside however
call arguments materialize their `NamedVariable`/`TypeAndValue` temporaries
before the `CreateCall`.

## Fix direction

Find where the p2 fix threads destructor calls for `?.` owned-string temps
(search for the null-conditional handling near `IsOwning`/`?.`/`OptionalChain`
in `MainListener_Expressions.cpp` or `LLVMBackend_OwnershipTemps.cpp`) and
make sure the same cleanup registration happens when the `?.` expression is
consumed as a call argument, not just when it is the top-level expression.
The general call-argument temp-cleanup path (used for e.g. `takeLen(a + b)`
today) may already have a hook to attach to - the null-conditional path
likely needs to register into that same list instead of (or in addition to)
its own end-of-statement cleanup.

## Regression test

Add a leg to whatever existing test covers the ?. owned-string-temp fix
(likely `Test/test_ownership.cb` or a null-safety test) with a
`takeLen(p?.label())`-shaped call and an `--atExit` leak assertion, once the
fix lands. Do not add a new test file.
