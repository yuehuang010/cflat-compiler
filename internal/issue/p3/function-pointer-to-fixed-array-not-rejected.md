# `function<T>[N]*` is silently accepted while `int[N]*` is correctly rejected

Filed 2026-07-31 by the review of `function-array-body-silently-truncated`. **Pre-existing**,
verified on the master binary at `64b6118`.

Severity: missing rejection of an invalid type. No wrong value has been demonstrated - the
probe only declares the variable - so this is a diagnostic gap, not a miscompile. It is filed
because it is a clean, cheap asymmetry with an existing correct message one branch away.

## Repro - the two halves

Correctly rejected:

```cflat
extern int main() { int[2]* p = nullptr; return 0; }
```
```
int_ptr_to_fixed_arr.cb(1,20): pointer to fixed array 'int[N]*' is not a valid type; pass
'int*' (a fixed array decays to a pointer to its first element).
```

Silently accepted:

```cflat
import "function.cb";
extern int main() { function<int(int)>[2]* p = nullptr; return 0; }
```
```
PASS: ptr_to_fixed_arr.cb
Checked 1 file(s), 0 failed.
```

## Root cause

The `functionPointerSpecifier` branch in `ParseDeclarationSpecifiers` `break`s out of the
specifier loop before reaching the `ArrayPtrOf(declSpec)` check that produces the message
above. Same structural cause as `function-array-body-silently-truncated`: that branch exits
early and skips the suffix handling every other type goes through.

## Fix direction

Apply the existing `ArrayPtrOf(declSpec)` rejection on the function-pointer branch, reusing the
message. Per CLAUDE.md this must be done in **both** copies of `ParseDeclarationSpecifiers()`
(the `ForwardRefScanner` copy and the `MainListener` copy, both in `cflat/MainListener.h`).

Best folded into whatever work next touches that branch - it is a two-line change there and
does not deserve its own round.

## Test coverage

None. Wants a `Test/errors/err_*.cb` leg pinning the message substring.

Related: [[interface-issue-queue]]
