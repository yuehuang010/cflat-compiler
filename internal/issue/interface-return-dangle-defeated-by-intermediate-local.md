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

The check belongs wherever the boxing decision ends up living rather than as another
value-shape walk bolted onto the return path - see the consolidation argument in
[[as-boxing-skips-ownership-transfer]]. A boxing site that recorded WHAT it boxed (frame
storage vs heap vs parameter) would answer this by construction, at the declaration of `r`,
without any def-use walking at the return.

## Related

- [[as-boxing-skips-ownership-transfer]] - the structural argument, and the reason the
  frame-lifetime check is currently a fourth copy of boxing bookkeeping.
- [[as-boxing-skips-pointer-shape-rejection]]
- The array-shaped-operand gap this used to link is FIXED; see the closed section of
  [[interface-issue-queue]].
