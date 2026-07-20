# `dictionary<K, unique V*>.add()` leaks the value on a duplicate key

Severity: guaranteed leak, silent. Found 2026-07-20 while migrating `hpc/btree.cb`; the same
defect was found and fixed in `btree` in that change, and is left here for `dictionary` /
`hashset`, which were migrated in Stage 6.

## Summary

Under borrow-by-default, `add(alias K key, V value)` takes a PLAIN parameter. When V substitutes
to `unique T*` that plain parameter is a synthesized move sink (`IsUniqueTypeArg`), so the
caller has already given the pointer up and holds no handle to it. On the duplicate-key path
`add` does a bare `return false` without storing OR freeing it, so the pointee leaks with no
way for the caller to recover it.

## Repro

```cflat
import "dictionary.cb";
import "diagnostic/heap_audit.cb";
int g = 0;
struct R { int v = 0; ~R() { g = g + 1; } };
extern int main()
{
    HeapAudit.enable();
    { dictionary<int, unique R*> d; d.add(1, new R()); d.add(1, new R()); }
    printf("dtor=%d leaks=%llu\n", g, HeapAudit.reportLeaks());  // dtor=1 leaks=1
    return 0;
}
```

Expected `dtor=2 leaks=0`: the refused second value must be freed by the callee.

## Site

`cflat/core/dictionary.cb`, `add(alias K key, V value)`:

```cflat
int insertSlot = _insertSlotFor(key);
if (insertSlot == -1) return false;      // <- `value` dropped, never freed
```

Check `hashset.cb`'s `add` for the same shape (a `unique` element refused as a duplicate).
The `move` overload has the same path, but there the drop is correct - a value type dropped
at scope exit runs its destructor; only the `unique` pointer leg needs the explicit free.

## Fix

Free the sink argument on the refusal path, as `btree.add()` now does:

```cflat
if (insertSlot == -1)
{
    // Duplicate: the container refuses the entry. A `unique` argument was already handed over
    // (a unique-substituted plain parameter is a synthesized move sink) and the caller kept no
    // handle to it, so it must be freed here or it leaks. Borrowed / value arguments are
    // left untouched.
    if const (is_unique(V)) { if (value != nullptr) delete value; }
    return false;
}
```

Note the `delete` must happen on a genuine local or a `move` parameter; `value` is a parameter
of this function, so it is fine directly. Add a regression leg to the `dict<unique ptr>` block
in `Test/test_collection_leaks.cb` asserting a refused duplicate frees exactly once.
