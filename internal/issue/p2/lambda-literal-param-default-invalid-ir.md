# A lambda literal as a parameter default emits invalid IR (module verification failure)

Filed 2026-08-06 by round-3 review of `fix/assign-gate`. Pre-existing on the base binary
(`68c78fc`); unrelated to the provenance gates, but lives in the same emitter the parameter-default
gate touched (`GenerateDefaultParamOverloads`, `MainListener_Statements.cpp` ~2300).

Severity: compile-time failure with an internal diagnostic (not silent), but the construct is
reasonable and works in the local decl-init spelling, so the asymmetry will surprise users.

## Repro

```cflat
import "function.cb";
int f(function<int(int)> cb = (int x) => x + 2) { return cb(1); }
extern int main() { printf("%d\n", f()); return 0; }
```
-> `Module verification failed: Found return instr that returns non-void in Function of void
return type`.

The same literal in a local decl-init works: `function<int(int)> g = (int x) => x + 2;` then
`g(1)` returns 3.

## Root cause

Not diagnosed. The default-value expression is parsed/emitted inside the synthesized forwarding
wrapper; the lambda body's `return` appears to be emitted into a function whose current return
type is void at that point. Trace where `GenerateDefaultParamOverloads` evaluates the
`assignmentExpression` and what function/builder context is current when the lambda literal is
lowered.

## Fix direction

Not planned. Diagnose the emission context first; the fix may be to lower the lambda literal in
the enclosing module context (as the decl-init path does) before building the wrapper call.
Whatever lands must also cover the struct-method twin (same emitter, called from the aggregates
path).

Related: [[interface-issue-queue]],
the `fix/fat-default` landed record in the queue file (accept-set overlap:
that gate must not regress or mask this issue; it verified this failure unchanged).
