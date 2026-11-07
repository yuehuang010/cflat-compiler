# A brace-initialized array view loses its length: foreach runs zero times

Found 2026-11-03 by the global-view review (box 10), PRE-EXISTING. Silent wrong behaviour.

```cflat
extern int main()
{
    int[] v = { 1, 2, 3 };
    int n = 0;
    for (int x in v) n++;
    printf("%d\n", n);   // prints 0, expected 3
    return 0;
}
```

The view's length slot holds -1, so foreach silently runs 0 times. Rebinding a view to an unknown-length view
(`void setv(int[] v) { g = v; }`, `g = pick()`) gives the same 0-iteration foreach. Expected: a brace-initialized view
carries its element count; a view whose length is genuinely unknown is refused by foreach with a located error rather
than iterating zero times.
