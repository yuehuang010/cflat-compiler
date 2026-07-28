# Named arguments are ignored on the interface call path

Filed 2026-07-27, found during review of the interface-overload-dispatch fix.
PRE-EXISTING: behaviour is identical on master `dcb9003` and on that fix branch.

Severity: SILENT MISCOMPILE (arguments bound to the wrong parameters, no diagnostic).

## Repro

```cflat
interface IF { int f(int a, int b); };
class N : IF { int f(int a, int b) { return a * 10 + b; } };
extern int main()
{
    N n;
    IF io = n;
    printf("iface=%d direct=%d\n", io.f(b: 2, a: 1), n.f(b: 2, a: 1));
    return 0;
}
```

Observed, exit 0, no diagnostic:

```
iface=21  direct=12
```

The direct call reorders correctly; the interface call evaluates positionally and
silently binds `b: 2` to `a` and `a: 1` to `b`.

## Root cause

`cflat/MainListener.h:19571-19623` - the interface call site builds a fresh `argVar`
and copies roughly 15 fields from `argNV`, but never copies
`argNV.TypeAndValue.VariableName`. `MatchFunction` (`LLVMBackend.h:16160`) keys named
arguments off exactly that field, so on the interface path it always sees zero named
arguments and returns an identity permutation.

## Fix direction

Propagate `VariableName` into the synthesized `argVar` at the interface call site so
`MatchFunction` sees the named arguments, then apply the returned permutation
consistently to ALL of the argument-emission, `DiagnoseExplicitMoveToBorrowParam` and
`ApplyMoveParamTransfer` loops - a permutation applied to one loop but not the others
binds move/borrow diagnostics against the wrong parameter, which is a silent ownership
bug rather than a visible one.

## Trap to clear first

The overload-dispatch fix added a multi-candidate loop that calls `MatchFunction` for
every arity-matching candidate. `MatchFunction` emits
`LogError("named argument '{}' does not match any parameter")` on a name miss, and the
loop's `continue` swallows the RESULT, not the DIAGNOSTIC. So the moment
`VariableName` starts propagating, a legal `io.go(alpha: 5)` against
`{go(int alpha), go(const char* beta)}` will hard-error from the LOSING candidate.
Candidate scoring must be made non-diagnostic before this issue is fixed. (That guard
is being added with the overload fix; verify it is in place first.)
