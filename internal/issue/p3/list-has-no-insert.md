# `list<T>` has no `insert(index, value)`

Filed 2026-08-21 from an external report (v0.11.0 issue 10). Reproduced on `39d4b38`:
`xs.insert(1, 2)` gives `Unknown identifier 'insert'`.

`cflat/core/list.cb` provides `add`, `set`, `get`, `operator[]`, `count`, `removeAt`, `take`,
`clear`, `copy`, `sort` - there is no positional insert, so ordered insertion means `add()` at the
end and then bubbling the new value into place with repeated `set()`, which is O(n) writes plus
hand-written index juggling at every call site.

## Fix direction

Add to `list<T>`, alongside the existing `add` overload pair:

```cflat
void insert(int index, T value);
void insert(int index, move T value);   // mirror of add(move T)
```

Semantics: `index == count()` appends; out-of-range goes through the existing `_checkBounds` path
with an "insert" op name. Implementation is `_grow()` if needed, then shift `[index, _size)` up by
one slot and store - the shift can reuse whatever `removeAt` already does in reverse, and must be
a raw slot move (no per-element copy/destructor) so it composes with `list<unique T*>`.

A sorted-insert helper is NOT needed as a separate API: with `insert` present, a caller pairs it
with the existing `sort` comparator, or a `binarySearch(value, compare)` returning the insertion
point would round it out cleanly if one is wanted later.

Cover the new method in the existing `Test/test_list*.cb` (front, middle, end, empty list,
out-of-range error leg, and a `unique` element leg).
