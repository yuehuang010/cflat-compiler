# Moving an owning value out of program-lifetime storage crashes when the code runs twice

Filed 2026-08-09 by the review of the fix that gave `static` locals their own storage. The
defect is **pre-existing on master** for file-scope globals; the fix makes it reachable
through one more spelling (a `static` local), which is why it is being recorded now.

Severity: **P2, memory unsafety** - segfault (exit 139) on the second execution.

## Repro (file-scope global - master and branch alike)

```cflat
string g = "abc";
int consume(move string s) { return (int)s.length(); }
int f() { return consume(move g); }
extern int main() { printf("%d\n", f()); printf("%d\n", f()); return 0; }
```

Prints `3`, then segfaults. Measured identically on master (`324d780`) and on the
static-local branch: `exit=139` both times, so nothing about the static-local work
introduced or worsened it.

## Same shape through a static local (branch only - `static` was ignored before)

```cflat
int consume(move string s) { return (int)s.length(); }
int f()
{
    static string s = "abc";
    return consume(move s);
}
extern int main() { printf("%d %d\n", f(), f()); return 0; }
```

`exit=139`. Pre-fix this could not happen: `static` was dropped, so `s` was a fresh local
per call - the move consumed a per-call value.

## Root cause (hypothesis, unverified)

`move` out of a NAMED local zeroes the source storage so the source's own destructor
no-ops. For program-lifetime storage the source is not re-initialized before the next
execution of the same statement: the second `move` hands the callee the zeroed (or
already-freed) value, and the callee's `move string` parameter destructor frees it again.
The frame-local case is safe only because the declaration re-runs each call.

## Fix direction

A `move` whose source is program-lifetime storage (a global, or a `static` local -
`NamedVariable::IsStaticLocal`) is either

- rejected at the move site, with a diagnostic pointing at `.copy()` for a string / owning
  value, since "move out of a variable that outlives the statement and is never
  re-initialized" has no sound reading; or
- accepted only where the compiler can prove a re-initialization dominates the next use,
  which is not currently modelled.

Prefer the reject: it is directly enumerable at the move site and matches the reject the
static-local fix already installs for address-of-a-local.
