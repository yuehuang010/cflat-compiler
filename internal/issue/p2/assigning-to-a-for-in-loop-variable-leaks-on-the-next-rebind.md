# Assigning to a range-`for` loop variable leaks the assigned buffer on the next rebind

Filed 2026-08-10 from the fix/forinstr round. Pre-existing on the CONTAINER leg (measured
identical on `01853aa` and fix/forinstr); on the fixed-array leg the same spelling crashed
(rc 133) before the fix, so the leak was unobservable there and is now visible because the
fixed-array leg was brought into line with the container leg.

Severity: leak of N-1 buffers, values all correct, no crash.

## Repro

```cflat
import "list.cb";
extern int main()
{
    list<string> l;                 // fixed-array spelling: string[2] dst; dst[0] = ...
    l.add("ab" + "cd");
    l.add("ef" + "gh");
    int n = 0;
    for (string s in l)
    {
        s = "zz" + "yy";            // s now OWNS a fresh buffer
        n += s == "zzyy" ? 1 : 0;
    }
    n += l.get(0) == "abcd" ? 10 : 0;
    n += l.get(1) == "efgh" ? 100 : 0;
    printf("n=%d\n", n);            // n=112 - every value correct
    return 0;
}
```

Measured (`leaks --atExit`), scratch/fi_y_assignlist.cb and scratch/fi_u_assign.cb:

| spelling | 01853aa | fix/forinstr |
|---|---|---|
| `list<string>` container | n=112, rc 0, 1 leak / 16 bytes | n=112, rc 0, 1 leak / 16 bytes |
| `string[2]` fixed array | rc 133, no output | n=112, rc 0, 1 leak / 16 bytes |

## Root cause

The loop variable's alloca is hoisted once for the whole loop and rebound at the top of every
iteration by the bare `compiler->CreateAssignment(elemVal, elemAlloca)` in the range-`for`
lowering (`MainListener_Statements.cpp`, shared by all three legs). That store does not drop
whatever the alloca already holds. Normally the alloca holds a cleared BORROW, so dropping it
would be a no-op - but if the BODY assigned an owning value to the loop variable, iteration
N+1's rebind overwrites a live owner and orphans its buffer. Only the last iteration's value is
freed, by the single dtor in `forRangeResume`.

## Fix direction

Drop the loop variable before the per-iteration rebind. Because every leg now stores a
bit-cleared borrow into that alloca, a destructor call there is a no-op on the normal path and
frees exactly the body-assigned owner on the leaking path.

Do NOT land this without an accept-set sweep first: the drop runs for EVERY element type, and
for an owning-pointer struct element (see
`p1/for-in-over-an-owning-struct-element-aliases-and-double-frees.md`, where the alloca holds a
LIVE aliased `unique T*` rather than a cleared borrow) it would delete the container's object
one iteration earlier instead of at `forRangeResume`. That cell is already broken, but the drop
must not be justified on the assumption that the alloca always holds a borrow.
