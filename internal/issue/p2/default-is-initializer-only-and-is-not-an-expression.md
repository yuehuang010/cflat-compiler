# `default` works only in an initializer: not assignable, not an expression

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 03).
Reproduced on `cd847a3`, Release.

## Repro A - not assignable

```cflat
struct P { int a = 0; };
extern int main() { P p = default; p = default; return 0; }
```

```
t_default.cb(2,37): error: mismatched input '=' expecting ';'
    extern int main() { P p = default; p = default; return 0; }
                                         ^
hint: missing ';' at end of statement
```

## Repro B - not an expression

```cflat
struct P { int a = 0; };
extern int main() { bool h = true; P q = h ? default : default; return 0; }
```

```
t_defexpr.cb(2,45): error: mismatched input 'default' expecting {'alignof', 'simd', ...}
```

The reporter's real shape was `AppHistory old = hasOld ? _appHistory.get(key) : default;`, which
had to become a declaration plus an `if`. Re-defaulting an accumulator between loop iterations
(repro A) had to become a fresh declaration in an inner scope.

## Why it is worth fixing

`default` reads like a VALUE everywhere else the language uses it - field initializers,
declarations, `= default` on a struct. Restricting it to declaration position makes every
re-defaulting site a mechanical rewrite, and the restriction is not discoverable: the diagnostics
blame the `=` or the following token and suggest a missing `;`, so nothing in the message says
"`default` is not valid here". See [[diagnostic-attribution-and-reserved-word-wording]] for the
general form of that mis-attribution.

## Fix direction

Promote `default` from an initializer-only production in `CFlat.g4` to a primary expression whose
type comes from the expected type at the use site - the same expected-type plumbing the brace
initializer already rides. That covers both repros at once: assignment RHS, ternary arm, call
argument, `return default;`.

Two constraints:

- **Where the expected type is unknown, reject with a message that says so** ("`default` needs a
  known target type here"), not with a parser mismatch.
- **Assignment must destruct first.** `p = default` on a struct with owning fields has to run the
  existing destruct-before-overwrite path or it leaks. Do not implement repro A as a raw memset.

## Regression test

Extend `Test/test_struct.cb` with `p = default` after mutation (assert fields are back to their
declared initializers), a ternary `default` arm, and an owning-field leg checked leak-clean.
