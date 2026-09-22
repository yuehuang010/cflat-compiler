Bucket: p3 (code quality; measured balanced and value-correct, but the predicate reasons by NAME)

# The by-value C++ parameter last-use scan identifies the parameter by its name text

Found 2026-09-21 in main-session review of by-value round 2 (fix/cpp-byvalue-param,
IsLastUseOfForeignCxxParam in cflat/MainListener.h).

The predicate decides "this bare occurrence is the parameter's last use, so move-construct from
it" by collecting every PrimaryExpression whose text equals the parameter NAME across the whole
function body and comparing token indices. A local that SHADOWS the parameter name is folded in:

```cflat
int shadow(cpptw.Twin t) { int a = take(t); { cpptw.Twin t = cpptw.Twin(50); a = a + take(t); } return a; }
// measured: the inner take(t) MOVES from the shadowing LOCAL (copy=2 move=1), because
// IsFunctionParameter("t") is true and that occurrence is the last one by token index.
```

Not wrong today: the local is never read afterwards (any later textual occurrence anywhere in
the body forces a copy, loops and lambdas force a copy) and it is destroyed as moved-from at
scope exit, so counts balance and values are right. It is an unintended widening (a plain local
at its last use also moves) and it rests on name text, so a future by-name mismatch (a global
of the same name read through a nested scope where the parameter is out of... etc.) has no type
model behind it.

Fix direction: resolve the occurrence to the NamedVariable (scope lookup at the use site) and
compare identity, not name; restrict to the actual parameter slot. Acceptance:
scratch/bvv_lastuse.cb in that worktree - the `shadow` row goes to copy=3 move=0 (or the
widening is ruled intended and documented), every other row unchanged.
