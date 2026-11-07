# `int[] b = move a;` of a brace-initialized view aborts at run time

## Summary

Moving an array view into another view and then iterating both compiles and aborts (exit 134). Found by the brview
round-2 review (box 10, 2026-11-03), PRE-EXISTING (master and the brview branch alike).

## Minimal repro

```cflat
extern int main() { int[] a = {1, 2, 3}; int[] b = move a; int n = 0; for (int x in a) n++; for (int x in b) n += 10; printf("%d\n", n); return 0; }
```

Observed (master c3d63f2f): prints 0, then exit 134. Expected: either `move` of a view is refused (a view does not own its
elements) or the moved-from view reads as empty and b iterates 3 times (prints 30) with no abort - needs a ruling on
what `move` means for a non-owning view; check what the abort is first (lldb).
