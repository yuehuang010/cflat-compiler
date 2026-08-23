# Fixed arrays have no safe length operation

## Summary

A fixed array has no `.length()` operation. The example therefore keeps a named
constant beside the fixed universe array and uses that bound.

## Minimal repro

```cflat
string[2] g = { "AAPL", "MSFT" };

extern int main()
{
    printf("%d\n", g.length());
    return 0;
}
```

## Observed vs expected

Observed: fixed arrays do not expose a length overload; the compiler reports
`no overload of 'length' matches the given arguments`.

Expected: fixed-array length should either return its compile-time bound or be
rejected consistently with a clear diagnostic.

## Root-cause hypothesis

Fixed-array bounds are not currently exposed as a compile-time or runtime
collection length operation.

## Fix direction

Reject `.length()` on fixed arrays with a targeted diagnostic, or add a compile-time
array length facility that returns the declared bound without emitting a call.
