# A bool variable passed to a vararg (printf "%d") prints garbage

Created: 2026-07-13 (found while spiking interface fields)

## Summary

Passing a `bool` VARIABLE to a variadic function (`printf("%d", b)`) prints
garbage. The same comparison written INLINE as the argument prints correctly, so
this is not a format-string mistake - it is a missing widening on the loaded
value.

This is a silent wrong-answer bug in ordinary user code, not a corner case: the
obvious way to print a flag is `printf("%d\n", myFlag);`.

## Repro

```cflat
extern int main()
{
    int a = 5;
    bool t = (a == 5);
    bool f = (a == 6);
    printf("direct : %d %d\n", t, f);          // -804465151 0   <-- WRONG
    printf("expr   : %d %d\n", a == 5, a == 6); // 1 0
    printf("as int : %d %d\n", (int)t, (int)f); // 1 0
    return 0;
}
```

Observed on x64/Release/cflat.exe (0.7), Windows:

```
direct : -804465151 0
expr   : 1 0
as int : 1 0
```

Note `false` happens to print `0` (the garbage upper bits are zero), so the bug
hides half the time - which is exactly how it survived this long.

## Root cause (hypothesis, needs confirming)

The inline-expression case yields an `i1` that gets widened on the way into the
call. The variable case loads the bool's storage and passes it without a
zero-extend to the vararg promotion width (int), so the upper bits are whatever
was in the stack slot. `(int)t` works because the explicit cast forces the
extension.

Look at the vararg argument-promotion path (default argument promotions for a
call with no prototype for that slot) and make a `bool`/`i1`/`i8` argument
zero-extend to `i32`, matching C's default argument promotions.

## Fix direction

In the call-argument lowering for variadic slots, apply the C default argument
promotions: any integer type narrower than `int` (including `bool`) must be
promoted to `int` with a ZERO-extend for `bool`. Add a regression case to
`Test/test_basic.cb` (or the closest existing test) printing a bool variable both
true and false.
