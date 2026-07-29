# `as` boxing into an interface skips RejectPointerShapedInterfaceUpcast

Filed 2026-07-28 by an adversarial review of the stack-value `as` fix.
PRE-EXISTING on the concrete-POINTER branch of `GenerateSafeCast`; not introduced by that fix.

Severity: accepts invalid source and silently boxes the wrong thing (no crash, wrong result).

## Repro

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };

int f(Circle[] v) { IShape s = v as IShape; return s.area(); }

extern int main()
{
    Circle[3] arr;
    arr[0].r = 2; arr[1].r = 3; arr[2].r = 4;
    printf("f=%d\n", f(arr));
    return 0;
}
```

Prints `f=4` - it silently boxed element 0 and dropped the rest of the view. The plain
spelling `IShape s = v;` is a hard error, so the `as` cast is what changes the outcome.

## Root cause

An array-view parameter is a pointer-shaped binding: it cannot carry a vtable, because the
fat pointer has exactly one data slot and a view has N elements. The assignment path rejects
it through `RejectPointerShapedInterfaceUpcast` (called at `MainListener.h:8151`), and the
return path calls the same guard at `MainListener.h:5730`.

`GenerateSafeCast` never calls it. `Circle[] v` lowers to a raw `ptr` whose elemType is the
`Circle` struct, so the operand takes the long-standing concrete-POINTER branch and boxes
element 0 with no shape check. (The value branch added for the stack-value crash fix does not
fire here: the operand is a pointer, not a struct aggregate.)

## Not closed by the frame-local return check

The stack-value fix rewired the RETURN path to reject an already-boxed fat pointer whose data
half is a frame-local alloca. That was expected to possibly close this too. **It does not**,
verified after the change: the repro above is an assignment, not a return, so the return path
is never consulted and it still prints `f=4`.

## Fix direction

Call `RejectPointerShapedInterfaceUpcast` from `GenerateSafeCast`'s concrete branch. It takes
a `TypeAndValue`, which `ParseTypeCheckExpression` currently discards - the same plumbing gap
described in [[as-boxing-skips-ownership-transfer]], whose "consolidate the three boxing
paths into one helper" recommendation covers this issue as well.

## Related

- [[as-boxing-skips-ownership-transfer]] - the structural fix direction lives there.
- [[as-cast-array-shaped-source-no-diagnostic]]
