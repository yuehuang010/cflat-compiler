# Interface implemented only inside a non-taken `if const` branch

Split out of `iface-rebox-emitted-before-registry-complete.md` when that issue was
fixed (deferred rebox emission). The parent issue asserted that deferring emission
to finalization "removes the need for the uncertainty marking and closes this too".
That claim is FALSE, so the sub-issue is re-filed here rather than retired.

Not a regression: master and the deferred-rebox build behave identically.

## Repro

```cflat
interface IOrph { int a(); };
interface IOrphChild : IOrph { int b(); };
class WinOnly : IOrph { ... };            // only on Windows
IOrph widen(IOrphChild c) { IOrph e = c; return e; }
extern int main() { return 0; }
```

with `WinOnly` declared inside `if const (__WINDOWS__)`, compiled on macOS:

```
ifconst_impl.cb(5,43): cannot convert to interface 'IOrph' - no class implements it
```

## Status

Undiagnosed - the direction of the symptom does not match what the parent issue
predicted. The parent described the uncertainty marking making the static check go
SILENT for such an interface (so a null-vtable crash goes uncaught). What is
actually observed is the opposite: a hard rejection.

Whether the rejection is even wrong is a judgement call worth settling first. On
this platform the class genuinely does not exist, so no value can ever reach the
conversion and "no class implements it" is literally true. The cost is that a
program which is correct on Windows fails to compile on macOS because of a
conversion that is dead there.

## Fix direction

Decide the intended semantics before writing code:

1. If a not-taken `if const` base clause should keep the conversion legal (dead
   code, no diagnostic), the finalization check in
   `ReportInterfaceReboxHasNoImplementor` needs to treat "declared in a non-taken
   `if const`" as uncertainty rather than absence.
2. If the rejection is correct, the parent issue's paragraph was simply wrong and
   this file should be deleted.

Do NOT solve it by propagating uncertainty up the interface inheritance chain -
that was tried and reverted: it lets one unrelated generic disable the
impossible-conversion guard for an entire ancestor chain, turning a compile error
into a null-vtable segfault.
