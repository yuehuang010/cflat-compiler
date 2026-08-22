# A `??` / `?:` join with ONE literal arm and one RUNTIME arm still slips the literal guard

Filed 2026-08-22 while fixing `string-literal-ternary-into-struct-pointer-slips-guard.md`
(the all-literal join case, now rejected by `LLVMBackend::JoinIsAllStringLiterals`). Measured on
macOS arm64, Release, at that fix's binary.

## Repro (compiles clean, SIGSEGV)

```cflat
import "string.cb";

extern int main()
{
    string? a = default;      // null
    string? b = a ?? "bb";    // rhs arm is a literal; lhs arm is a runtime load
    return (int)b.length();   // SIGSEGV - b points at the literal's bytes
}
```

The `?:` twin is the same shape:

```cflat
struct Point { int x = 0; int y = 0; };
extern int main() { bool g = true; Point pt = default; Point* p = g ? "aaaa" : &pt; return p.x; }
```

with `g == true` (reads 'a' bytes as a `Point`).

## Root cause

`JoinIsAllStringLiterals` proves the reject with an EVERY-arm quantifier: every arm must be a
string literal, or a null constant (which carries no data and reads neutral, exactly as
`JoinArmDataKind` reads null). An arm that is a runtime load is UNPROVEN, so the whole join is
unproven and the store is accepted - even though the literal arm on its own branch is exactly the
store the gate exists to reject.

An ANY-arm quantifier would catch it, and would also reject `g ? &pt : "zz"`, which master
compiles and runs correctly on its pointer arm. That accept-set is frozen as value legs in
`Test/test_operators.cb::stringLiteralJoinAcceptLegs` and a flip of the quantifier turns them into
hard errors, so the change is not a one-line polarity swap.

## Fix direction

Decide whether a literal arm is wrong PER BRANCH rather than per join. The store is definitely
wrong on the literal arm's branch in every one of these shapes - there is no reading of
`S* p = c ? "lit" : q;` under which the literal branch yields a usable `S*`. If that ruling is
taken, the gate becomes "reject when ANY arm is a proven string literal" and the four mixed-arm
accept legs in `Test/test_operators.cb` have to be retired in the SAME change (they are the
programs the current polarity exists to protect), with the diagnostic naming the arm rather than
the whole store. If it is not taken, this stays permanent residue and should be recorded as such.
