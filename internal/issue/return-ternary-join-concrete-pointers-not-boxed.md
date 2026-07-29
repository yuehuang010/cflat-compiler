# Returning a `?:` join of concrete pointers as an interface: verifier failure, no diagnostic

Filed 2026-07-29 while checking `as`-vs-plain parity during the boxing consolidation.
PRE-EXISTING: identical on the pre-change binary.

Severity: compile aborts with a raw LLVM verifier dump and no source location. Legal-looking
source, no diagnostic pointing at it.

## Repro

```cflat
interface IShape { int area(); };
class Square : IShape { int s = 0; int area() { return s * s; } };

IShape pick(int c)
{
    Square* x = new Square(); x.s = 3;
    Square* y = new Square(); y.s = 3;
    return c > 0 ? x : y;      // plain spelling, no 'as'
}

extern int main() { IShape a = pick(1); printf("pick=%d\n", a.area()); return 0; }
```

```
Module verification failed:
Function return type does not match operand type of return inst!
  ret ptr %ternary
 %__iface_fat_ptr = type { ptr, ptr }
Error: module verification failed.
```

## Root cause

Not fully diagnosed. The RETURN path boxes a concrete implementer pointer into the
interface fat pointer, but a `?:` join arrives as a phi/select of raw `ptr` values with no
single `TypeName` on the returning `NamedVariable`, so the boxing block is skipped and the
bare `ptr` reaches `CreateReturnCall`.

The ASSIGNMENT path handles the same shape correctly through
`UpcastTernaryPhiToInterface` (`MainListener.h:10662`), which boxes each arm in its own
branch - `IShape s = c > 0 ? x : y;` compiles and runs. The return path has no equivalent
call.

## Note on the `as` spelling

`return c > 0 ? (x as IShape) : (y as IShape);` from a non-`move` function is now correctly
REJECTED with the ownership-escape diagnostic, and the `move IShape` spelling of it works
and returns the right value. So the `as` spelling is currently better than the plain one
for this shape. That asymmetry is this issue.

## Fix direction

Call `UpcastTernaryPhiToInterface` from the return path the way the assignment path does,
before the frame-lifetime and ownership checks run, so the returned value is a fat pointer
by the time those checks inspect it. Then re-check that the ownership-escape rejection and
the frame-local dangle diagnostic still fire correctly on the boxed result - a heap arm
must still be rejected from a non-`move` function, and a frame-local arm must still be
rejected as a dangle.

Per CLAUDE.md, a verifier abort reachable from plain source must become a proper `LogError`
once the root cause is known - so even the shapes that cannot be boxed need a diagnostic
rather than a verifier dump.

## Related

[[interface-boxing-guards-are-binding-dependent]], [[interface-issue-queue]]
