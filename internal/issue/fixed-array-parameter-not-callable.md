# A function with a fixed-array parameter 'T[N]' can never be called

Filed 2026-07-29 while fixing `global-primitive-array-boxed-into-interface`. PRE-EXISTING
and unrelated to that fix: identical on `df32dd8` and on the fix commit. NOT
interface-related - filed here because `internal/issue/` is the only queue.

Severity: FALSE REJECTION of a legal-looking declaration. The definition is accepted; every
call to it fails overload resolution. No miscompile.

## Repro

```cflat
int sumFixed(int[3] a) { return a[0] + a[1] + a[2]; }

extern int main()
{
    int[3] q; q[0] = 1; q[1] = 2; q[2] = 3;
    printf("%d\n", sumFixed(q));
    return 0;
}
```

Both binaries, exit 1:

```
no overload of 'sumFixed' matches the given arguments.
  Call arguments (1):
    [0] ptr <unnamed>
  Candidates (1):
    _sumFixed_int_int_(int a)
  Argument mismatch detail (single resolved candidate: _sumFixed_int_int_):
    [0] arg=ptr  param=int
```

The generic spelling fails the same way: `T firstOf<T>(T[4] a)` mangles to
`_firstOf__int_int_int_(int a)`.

The array-VIEW spelling is the working alternative: `int sum(int[] v, int n)` and
`T firstOf<T>(T[] a)` both declare, resolve and run correctly.

## Root cause

Not diagnosed, but the mangled candidate name is the evidence: `_sumFixed_int_int_(int a)`
shows the parameter registered as a bare `int`. The PARAMETER declaration drops the array
extent (and the pointer-ness that comes with the decay), so the signature the call site is
matched against never had an array in it. The argument arrives correctly as a `ptr`, so it
is the parameter side that is wrong, not the call side.

## Fix direction

Either lower a `T[N]` parameter the way C does - decay it to `T*` (or to the existing `T[]`
view) at registration time, so the signature the call site matches carries a pointer - or
reject the declaration outright with a `LogError` pointing at `T[]`. What must not remain is
today's state, where the declaration is accepted and only the CALL is rejected, with a
message that describes an `int` parameter the user never wrote.
