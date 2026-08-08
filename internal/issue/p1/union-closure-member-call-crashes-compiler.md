# Calling a closure member of a `union` crashes the COMPILER

Filed 2026-08-07 by the round-1 review of `fix/fat-default` (informational sweep). Pre-existing;
identical with and without the provenance gates.

Severity: compiler crash (fat member) / module verification failure (thin member). Nothing
reaches runtime, so there is no memory-unsafe accept - but a crash is never an acceptable
diagnostic, and per the debugging workflow a diagnosed LLVM-assert/crash path wants a proper
error message.

## Repro

```cflat
import "function.cb";
int addOne(int x) { return x + 1; }
union UD { Lambda<int(int)> f = addOne; int i; };
extern int main() { UD u = default; printf("%d\n", u.f(1)); return 0; }
```
-> the `-o` compile itself exits 139 (compiler SIGSEGV), even for this perfectly legal
named-function source.

The thin twin (`union UT { function<int(int)> f = addOne; int i; };` then `u.f(1)`) dies in
module verification instead: `Invalid bitcast %cfn_ptr = bitcast %UT %2 to ptr`.

Also: a data-pointer default on a union closure member (`union UD { Lambda<int(int)> f = gvp;
... }`) compiles clean with NO provenance reject - the union member-default path does not run
either assign-provenance gate. Ungated but currently unreachable at runtime because the call
crashes the compile first; if the call path is fixed, the gate must be wired for union members
in the same change.

## Root cause

Not fully diagnosed. The member-access lowering for a union closure member appears to hand the
whole union value where a closure pointer is expected (`bitcast %UT %2 to ptr`), suggesting the
union member-call path never learned about closure members at all.

## Fix direction

Diagnose the union member-call lowering for closure-typed members; either implement it or reject
closure members in unions with a located LogError. Whichever lands must also wire
`CheckThinFnPtrAssignProvenance` / `CheckFatClosureAssignProvenance` into the union
member-default path (see above).

Related: [[interface-issue-queue]].
