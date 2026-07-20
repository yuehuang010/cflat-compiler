# User-defined `copy()` result leaks when stored in a container

## Summary

A value type whose `copy()` is HAND-WRITTEN (returning `move v`) leaks its owned buffer once
the copy is stored in a container slot. The same type with a synthesized memberwise copy does
not leak. Affects `list`, `dictionary` and `hashset` identically, so it is not a container bug.

## Repro

```cflat
import "dictionary.cb";
import "diagnostic/heap_audit.cb";
struct Plain { string n = default; };
struct Val   { string n = default; Val copy() { Val v; v.n = n.copy(); return move v; } };

extern int main()
{
    HeapAudit.enable();
    { list<Plain> l = default; Plain a = default; a.n = "aa"+"bb"; l.add(a); }
    printf("list<Plain> LEAKS=%llu\n", HeapAudit.reportLeaks());   // 0
    { dictionary<int,Plain> d = default; Plain a = default; a.n = "cc"+"dd"; d.add(1, a); }
    printf("dict<Plain> LEAKS=%llu\n", HeapAudit.reportLeaks());   // 0
    { list<Val> l = default; Val a = default; a.n = "ee"+"ff"; l.add(a); }
    printf("list<Val> LEAKS=%llu\n", HeapAudit.reportLeaks());     // 1  <-- LEAK
    return 0;
}
```

A bare `Val` LOCAL does not leak - only the container-stored copy does.

## Root cause (not yet confirmed)

The leak trace bottoms out in `_copy_string_string_` called from the user `copy()`, reached
from the container's `_placeAt` / `_placeValueAt`. The slot's element destructor
(`_data[i].~()` / `_values[i].~()`) evidently does not free the string field of a value
produced by a user `copy()`, while it does for a memberwise-copied one. Suspect the
`move`-returned temporary's ownership flags are not carried onto the stored slot value.

## Fix direction

Compare the destructor synthesis / ownership flags for a `move`-returned struct from a user
`copy()` against the memberwise path. Confirm whether the slot value is ever registered as
owning at all.

## Discovered

2026-07-20, during the `hashset`/`dictionary` borrow-by-default migration (unique-ownership
plan item 8). Pre-existing on unmodified `list.cb`, so it did not block that work.
