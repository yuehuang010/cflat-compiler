# An owning interface box produced by a ternary leaks as a receiver

## Summary

An owning interface value produced by a ternary is not released when it is used directly as a
method receiver. This is independent of the owning-struct receiver fix in this round: the
interface box has its own fat-pointer ownership path and is not added to the pending cleanup set.

## Minimal repro

```cflat
int dtorCount = 0;

interface IBox { int value(); };

class Box : IBox
{
    int n = 0;
    Box(int x) { n = x; }
    int value() { return n; }
    ~Box() { dtorCount = dtorCount + 1; }
};

move IBox makeBox(int n) { return new Box(n); }
bool choose(bool value) { return value; }

int main()
{
    int got = (choose(false) ? makeBox(1) : makeBox(2)).value();
    printf("%d %d\n", got, dtorCount);
    return 0;
}
```

## Observed vs expected

The pre-fix and post-fix binaries both print `2 0`. The expected output is `2 1`: the selected
owning interface box should be released after the receiver call.

## Root-cause hypothesis

The ternary produces a fat interface value whose concrete ownership is represented by the box
metadata rather than by an owning-struct PHI temp. Receiver lowering spills or dispatches the fat
value but does not register the selected interface box for cleanup.

## Fix direction

Extend the interface-box ownership ledger used by ternary joins and receiver lowering. Register the
selected concrete box exactly once, preserve null-arm behavior, and avoid registering a box that
was moved into an owning destination or consumed by a move receiver.
