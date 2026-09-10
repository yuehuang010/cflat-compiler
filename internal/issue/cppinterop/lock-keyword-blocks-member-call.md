# `lock` is a grammar keyword, so `m.lock()` is unparseable

Found 2026-09-09 by the std header coverage spike ([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 5).
Small and self-contained; blocks the primary member of `std.mutex` and `std.shared_mutex`.

## Repro

```cflat
import cpp "mutex";
extern int main() { std.mutex m = default; m.lock(); m.unlock(); return 0; }
```

    error: cannot understand the code at 'm.lock'

`m.try_lock()` and `m.unlock()` on the same object parse fine, so only the exact spelling `lock`
is affected.

## Root cause

`lock` is a statement keyword in the grammar (`cflat/CFlat.g4:621`, `lock '(' lockArgList ')'`).
After the `.` of a member access, the lexer still hands back the `lock` token and the member-access
rule has no production for it.

The same hazard exists for every other hard keyword that is also a plausible C++ member name;
`lock` is just the first one a real header hit.

## Fix direction

Let the member-access position accept a keyword token as an identifier. Prefer a grammar-level fix
covering all keywords in that position over a one-off for `lock`, since the next collision is a
matter of which header gets imported next.

Acceptance: the repro compiles and runs to exit 0, asserted in `Test/test_cpp_interop.cb`; a
regression case covers at least one other keyword used as a member name.
