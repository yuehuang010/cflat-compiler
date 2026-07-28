# 'as' cast of a stack value to an interface crashes the compiler

Filed 2026-07-27, found incidentally during review of the interface-array-view fix.
PRE-EXISTING on master `dcb9003` - not introduced by that fix.

Severity: COMPILER CRASH (SIGSEGV, no diagnostic) reachable from plain source.

## Repro

```cflat
interface IMore { int more(); };
// ...
IMore m = someStackValue as IMore;
```

The compiler segfaults. Reduce further before starting: the reviewer hit this while
probing something else and did not minimize it. Capture the exact minimal shape,
including whether the source value being a STACK value (rather than a `new`'d pointer)
is actually load-bearing, and whether the target interface being unimplemented by the
source type matters.

## Root cause

Not diagnosed.

## Fix direction

1. Minimize the repro and capture where it faults (build Debug and get a stack trace -
   `CompilerManager.h` installs crash handlers that dump compiler state on assert or
   abort, and `-v` gives detailed diagnostics).
2. Per CLAUDE.md, a crash a user can reach from source must become a proper `LogError`
   if the construct is genuinely unsupported, rather than faulting. Decide whether the
   cast should be supported (produce a correctly boxed fat value, or a null one when the
   type does not implement the interface, matching how `as` behaves elsewhere) or
   rejected with a clear message.
3. Note `as` returning null on a failed interface cast IS existing behaviour used
   elsewhere - a probe in the same review relied on `v as IOther` yielding null when the
   class does not implement `IOther`. So the supported path likely already exists for
   some operand shapes and this is a gap in one of them.

## Related

[[iface-null-conditional-field-no-guard]] - a null interface produced by a failed `as`
then segfaults at runtime on `?.` field access, which is a separate filed issue.
