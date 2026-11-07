# `delete` of a brace-initialized array view compiles and aborts

Found 2026-11-03 by the global-view review (box 10), PRE-EXISTING.

```cflat
extern int main() { int[] g = { 1, 2 }; delete g; return 0; }   // compiles, aborts 134
```

A view bound from a named fixed array is refused with "cannot delete array-view"; a brace-initialized view (local, or the
global form landed 2026-11-03) must get the same refusal - its backing storage is not heap-owned.
