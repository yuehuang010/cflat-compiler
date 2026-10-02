# Closure capture of a fixed array of owning elements is not deep-copied

Pre-existing on master (found by the N50 round-4 review, 2026-10-02). A closure that captures `string[2] a`
by value loses the values when the closure is copied or moved:

```cflat
string[2] a = { "abc...", "..." };
Lambda<int(int)> f = (int i) => { return a[i].length(); };
int before = f(0);        // 38
Lambda<int(int)> g = f;
// g(0) and f(0) both return 0 - expected 38
```

ASan clean, identical on master and the N50 branch. Through N50 the same shape reaches C++
(std::function destination returns 0, clang 38).

Likely cause: `isOwningCap` (MainListener_PostfixExpression.cpp ~8636) treats arrays as non-owning, so
copy/move of the environment never deep-copies the elements. Fix direction: treat a fixed array whose
element type owns as an owning capture (per-element copy/move), or refuse the capture until then.
Also P3: a unique<T> capture refusal says "captures by reference ... capture a pointer instead" on the
`std.function g = move f` and deduced-callable paths (the `std.function<...>(move f)` path says move-only).
