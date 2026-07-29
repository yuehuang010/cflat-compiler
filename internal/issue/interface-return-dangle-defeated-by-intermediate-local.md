# One intermediate local defeats the interface return-dangle guard

Filed 2026-07-28 by an adversarial review of the stack-value `as` fix.
PRE-EXISTING: both spellings behave identically, so this is not something `as` introduced.

Severity: accepted source, no diagnostic, returns a pointer into the dead frame.

## Repro

```cflat
interface IShape { int area(); };
class Square : IShape { int s = 0; int area() { return s * s; } };

// Both of these compile clean and both dangle.
IShape viaCast(int n)  { Square loc; loc.s = n; IShape r = loc as IShape; return r; }
IShape viaPlain(int n) { Square loc; loc.s = n; IShape r = loc;           return r; }

int clobber() { int[64] a; int i = 0; while (i < 64) { a[i] = 0x5A5A5A; i = i + 1; } return a[7]; }

extern int main()
{
    IShape v = viaCast(7);
    int j = clobber();
    printf("area=%d (expect 49) j=%d\n", v.area(), j);   // prints garbage
    return 0;
}
```

Prints `area=-1490327644`. Returning the same expression directly - `return loc as IShape;`
or `return loc;` - is correctly rejected.

## Root cause

The return-path frame-lifetime check (`FrameLocalDataOfFatValue`, `MainListener.h:5066`)
inspects the returned VALUE. It follows `insertvalue` chains and `?:` joins back to the data
pointer that was boxed, and it deliberately stops at a load: tracing through loads is what
would make heap and by-reference shapes look frame-local and produce false rejections.

Binding the boxed value to a local first means the returned expression is a plain load of
that local's slot, so the walk bails on the first instruction and the guard never engages.
Closing this needs store-to-slot reasoning (which store reaches this load), not a deeper
value walk.

## Practical consequence

A user who hits the return-dangle error can make it disappear by inserting a local:

```cflat
IShape f() { Square s; return s as IShape; }              // rejected
IShape f() { Square s; IShape r = s as IShape; return r; } // accepted, still dangles
```

That is the worst shape for a diagnostic to have - it teaches the wrong workaround. Worth
weighting when prioritising this.

## Fix direction

**The prerequisite now EXISTS - this is no longer a design problem, only a wiring one.**
The boxing consolidation added exactly the thing this issue asked for: a provenance ledger,
`LLVMBackend::interfaceBoxRecords_`, written by `BoxConcreteIntoInterface`
(`MainListener.h:9969`) and keyed on BOTH the produced fat value and its data half. Each
record carries `{FatValue, DataPointer, SourceClassName, InterfaceName, Source,
OwnershipTransferred}` where `Source` is one of Unknown / FrameStorage / Heap / Parameter /
Global.

It is deliberately NOT retired by `FlushOwnedTemps` - unlike its sibling ledgers - precisely
so a record written at the declaration of `r` is still there at the `return` in a later
statement, which is this issue's shape.

So the fix is: at the return path, look the returned value up in the ledger instead of
walking IR. A record with `Source == FrameStorage` is the dangle, whether the value came
straight from the boxing site or through an intermediate local. That answers
`IShape r = loc as IShape; return r;` by construction, because the fact was recorded where
the boxing happened rather than recovered from the shape of the returned expression.

`FrameLocalDataOfFatValue` (`MainListener.h:5129`) can then be reduced to a fallback for
values with no ledger record (a fat pointer that arrived from a call, say), or removed if
the ledger proves complete.

**Read [[interface-boxing-sites-not-fully-consolidated]] BEFORE starting.** This change adds
the ledger's SECOND consumer, and unlike the first it will not sit behind the
`FrameLocalDataOfFatValue(right) == nullptr` gate that currently makes two known sharp edges
unreachable: `ClassifyInterfaceBoxSource` tests ownership before storage shape (so a
by-value class local with an owning binding can be labelled `Heap` with an alloca data
pointer), and `FindInterfaceBoxByDataPointer` returns the first of several records sharing a
data pointer. Both must be resolved as part of this work.

Note also that the ledger only has a record when the source had a NamedVariable - see
[[interface-boxing-guards-are-binding-dependent]] for the shapes that produce no binding.

## Related

- [[interface-boxing-sites-not-fully-consolidated]] - REQUIRED READING; the ledger sharp
  edges this change would expose.
- [[interface-boxing-guards-are-binding-dependent]] - the shapes the ledger cannot see.
- The two `as`-boxing issues this used to link (ownership transfer, pointer-shape rejection)
  are FIXED; see the closed section of [[interface-issue-queue]].
