# A stack fixed array `S[N] a = default;` skips every field initializer of the element struct

Filed 2026-08-07 by the round-1 review of `fix/fat-widen`. Pre-existing; measured identical on
`a7bdc31` and on `fix/fat-widen`.

Severity: silent wrong value (zeroed fields), escalating to a crash when the skipped field is a
closure.

## Repro

```cflat
struct S { int k = 7; };
extern int main() { S[2] a = default; printf("%d %d\n", a[0].k, a[1].k); return 0; }
```
-> compiles rc 0, prints `0 0` (expected `7 7`).

```cflat
import "function.cb";
int addOne(int x) { return x + 1; }
struct S { Lambda<int(int)> f = addOne; };
extern int main() { S[2] a = default; printf("%d\n", a[0].f(30)); return 0; }
```
-> compiles rc 0, run rc 139 (SIGSEGV) - the zeroed closure is called.

The heap spelling diverges: `new S[2]` DOES run field initializers (and after `fix/fat-widen`
widens closure defaults correctly, returning 31). A single stack `S s = default;` also runs them.
Only the stack fixed-array spelling zero-fills.

## Root cause

Not traced to a line. The fixed-array `= default` lowering zero-initializes the whole array
storage without invoking the element type's synthesized default constructor per element - same
family as the GLOBAL-scope `S g = default;` zero-init (see the `fix/fat-widen` landed record's
incidental note) and `internal/issue/p3/global-struct-no-initializer-ignores-field-defaults.md`,
but at local scope, where the single-instance spelling DOES honour defaults, so the asymmetry is
user-visible within one function.

## Fix direction

Make the stack fixed-array `= default` path call the element's default ctor per element (loop or
unrolled), matching `new S[N]`. Accept-set first: `int[N] = default` (all zeros, correct today,
must stay), element types with no field defaults (zero == ctor result, must stay byte-identical),
struct elements with dtors (each element must be registered for destruction exactly once).

Related: [[interface-issue-queue]], [[fixed-array-field-brace-default-discarded]] (the FIELD
position of the fixed-array family).
