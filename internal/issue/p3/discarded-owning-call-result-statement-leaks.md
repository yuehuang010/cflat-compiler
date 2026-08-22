# A discarded owning call result at statement level leaks

Filed 2026-08-22 while fixing the '?.' owned-temp gap. Reproduces on master and is
INDEPENDENT of '?.' - the '?.' and non-'?.' spellings leak identically, which is why
it was left out of that fix.

## Repro

```cflat
struct P
{
    string name = default;
    string label() { return this->name + "_suffix_long_enough"; }
};

int main()
{
    P q; q.name = "ab";
    q.label();          // result discarded
    return 0;
}
```

`leaks --atExit` reports 1 leak / 32 bytes. The '?.' spelling `p?.label();` leaks the
same 1 / 32 - measured pre- and post-fix on both spellings, so the merge is not involved.

## Root cause (hypothesis, not measured to the line)

A string-returning call whose result is never bound and never passed anywhere reaches no
registration site: the owned-string temp is registered either by the CALL (only when the
candidate is ReturnsOwned - a plain user method is not) or by an ARGUMENT / operand /
ternary-arm site, and a bare expression statement is none of those. `FlushOwnedTemps` at
the block-item boundary therefore has nothing to free.

## Fix direction

Register the produced temp at the expression-STATEMENT boundary with the same predicate
the argument sites use (`LLVMBackend::IsProducedTempValue` plus the string/closure/owning
-struct type test), i.e. treat "discarded" as one more consuming position. Check the
owning-struct and closure spellings at the same time.

## Regression test

Not observable in-language for `string` (the value is correct either way). Use an owning
STRUCT with a counting destructor, as `testNullConditionalOwnedTemp` in
`Test/test_basic.cb` does, and assert the destructor ran once.
