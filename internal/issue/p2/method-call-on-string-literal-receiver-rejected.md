# A method call on a STRING LITERAL receiver is rejected, and the diagnostic names the literal as a function

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 05).
Reproduced on `cd847a3`, Release.

## Repro

```cflat
import "string.cb";
extern int main() { string s = "".copy(); printf("%d\n", s.length()); return 0; }
```

```
t_lit.cb(2,31): the function '""' is not known.
```

Any method on a literal receiver fails the same way - `"abc".length()`, `"x".copy()`. The
workaround is a named local (`string e = ""; e.copy();`).

Two defects in one:

1. **The call is rejected.** A string literal already lowers to a `string` value, so method
   dispatch should find the same overload set a named `string` local finds. Literal receivers are
   ordinary in a language with UFCS: seeding a builder, returning an empty sentinel,
   `"".join(parts)`.
2. **The diagnostic is wrong in kind.** It renders the receiver literal as if it were the callee
   name, so the message points the reader at a nonexistent function `""` rather than saying a
   literal cannot be a receiver. Even if (1) is deferred, this wording should be fixed.

## Fix direction

In the postfix-chain handling in `MainListener.h`, a `StringLiteral` primary needs to materialize
its `string` temporary and become the receiver for the following `.method()` link, the same way a
named local does. This is the literal-receiver case of the same "the previous link is not re-seated
as the receiver" family as [[chained-method-call-on-primitive-result-uses-base-receiver]] - check
whether one fix covers both before writing two.

Ownership note: the temporary is an OWNED string, so it must be registered as an owned temp and
destructed at the end of the full expression, or this trades a rejected program for a leak. That is
the same choke point the 2026-06-18 owned-temp-as-call-argument fix uses.

## Regression test

Extend `Test/test_string.cb` with `"".copy()` and `"abc".length() == 3`, plus a leak-clean
assertion under that file's existing HeapAudit leg if it has one.
