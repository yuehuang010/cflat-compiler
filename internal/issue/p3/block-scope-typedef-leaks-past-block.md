# A block-scope typedef stays visible after its block and shadows the global one

Found 2026-11-03 by the fntype review (box 10), PRE-EXISTING. Silent wrong type.

```cflat
typedef int A;
int f(int x) { return 1; }
int f(double x) { return 2; }
extern int main()
{
    { typedef double A; }
    A y = 1.5;                       // CFlat: double, clang: int
    printf("%d\n", (int)(y * 2));    // CFlat 3, clang 2
    function<int(A)> h = f;          // CFlat binds f(double), clang f(int)
    printf("%d\n", h(1));
    return 0;
}
```

Fix direction: typedef / using declared inside a block must be scoped to that block (pop with the scope), so lookup after
the block finds the global alias.
