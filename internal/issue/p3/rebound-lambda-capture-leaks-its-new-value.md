# Rebinding a lambda's by-value owning capture leaks the value it is rebound to

Filed 2026-08-09 by the fix for [[lambda-returning-a-captured-string-double-frees]], which
measured this cell moving from a double free to a leak. Recorded because the trade is
deliberate, not because it is acceptable forever.

Severity: **P3, leak only.** No unsafety, no wrong value.

## Repro

```cflat
extern int main()
{
    string s = "xyz";
    Lambda<string()> g = () => { s = s + "!"; return s; };
    string r1 = g();
    string r2 = g();
    printf("r1=%s r2=%s s=%s\n", r1.data(), r2.data(), s.data());
    return 0;
}
```

Prints `r1=xyz! r2=xyz! s=xyz`, exit 0. `leaks --atExit` reports **2 leaks / 32 bytes**
(one per call: the `s + "!"` result). On the pre-fix binary the same program aborted rc 133
and reported 0 leaks - it "reclaimed" those bytes by freeing storage the env still owned,
so the old zero was a memory-unsafety artifact, not correctness.

## Root cause

`MainListener_PostfixExpression.cpp`'s capture-unpack marks a by-value owning capture's
unpacked local `IsAliasBorrow`, which suppresses its scope-exit destructor. That is correct
while the local still holds the env's buffer - the env's cleanup fn frees it exactly once.
It is wrong once the body REBINDS the local to a value the body itself owns: nothing then
destructs the new value, and the env keeps (and frees) its original copy.

The same suppression exists for the mixed-`?:`-join borrow local, with the same accepted
leak, so this is the established trade rather than a new one.

## Fix direction

The local needs a per-rebind ownership fact, not a whole-scope one: retire the borrow
suppression when the local is provably rebound to an owned value in the current block, the
way `BorrowProofRetiredByRebind` (`ReboundToOwnedValue && ReboundBlock == current block`)
already does for the borrow-forward policy. Do NOT simply drop `IsAliasBorrow` at the
unpack - that restores the double free the capture fix removed, which is exactly the
polarity the landed record warns about.

Related: [[boxed-join-proof-never-retires-a-rebound-arm]].
