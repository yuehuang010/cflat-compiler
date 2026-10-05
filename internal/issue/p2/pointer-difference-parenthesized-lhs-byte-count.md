# Pointer difference with a parenthesized pointer-arithmetic LHS yields a byte count (silent wrong value)

`(ip + 2) - ip` evaluates to 8 for `int* ip`; `ip + 2 - ip` gives 2 (C: 2 in both). Pre-existing on master
33866560, found by the T24 review (2026-10-03).

## Repro

```cflat
extern int main()
{
    int[4] a = {1, 2, 3, 4};
    int* ip = &a[0];
    long d1 = (ip + 2) - ip;
    long d2 = ip + 2 - ip;
    printf("%ld %ld\n", d1, d2);   // prints "8 2", expected "2 2"
    return d1 == 2 && d2 == 2 ? 0 : 1;
}
```

## Root cause (GUESS)

The parenthesized `ip + 2` result loses its pointee type (same producer gap as
p2/cpp-pointer-arithmetic-result-type-not-propagated), so the subtraction is lowered as integer subtraction of
two addresses instead of a ptrdiff divided by sizeof(int).

## Fix direction

Give the `+`/`-` pointer result its full pointer type (pointee included) where it is produced; the difference
lowering then divides by the element size. Add value legs (int, double, struct element sizes; parenthesized and
not) to an existing pointer-arithmetic test.
