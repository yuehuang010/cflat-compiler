# Compiler crash after a scoped expect_error inside a lambda returning a struct whose ctor builds a lambda

Severity p2 (internal compiler error after PASS). Found by the T345 r2 review (2026-10-20); pre-existing on master (exit 139).
Repro: scratch/repro_keep/box1020/t345_followup/recover_lambda.cb

```cflat
struct S { int id = 0; Lambda<int()> f = default; S() { this.f = () => { return this.id; }; } };
int main() { expect_error("Undefined variable missing.") { Lambda<S()> f = () => { return missing; }; } S s = S(); s.id = 9; return s.f(); }
```
Prints "PASS: expected error received" then "cflat internal compiler error". Likely transient lambda / sret state left by the aborted lambda
body (cf. T345 r3 CxxTransientMark rewind of pendingCxxSretDest_ at the 3 recovery sites) - check which state the rewind does not cover.
Fix: the recovery sites must restore every transient lambda-emission state; add the shape to an existing Test/errors/ lambda file.
