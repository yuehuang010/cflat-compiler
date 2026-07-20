# `return move <named local>` from a `move`-returning function leaks the struct's owning FIELDS

Severity: silent leak. Found 2026-07-20 during the `hpc/btree.cb` borrow-by-default migration
(a `move V _dupValue(V value)` helper leaked one string buffer per copying insert).

## Summary

A function declared `move S f()` that builds a struct local and returns it with
`return move r;` loses the owning buffers held in `r`'s FIELDS. The value arrives at the caller
usable (the field reads correctly), but nothing ever frees it: the callee's scope-exit
destructor is correctly suppressed by the move, and the caller's receiving local is apparently
not marked owning, so neither side runs the destructor.

The same function with a PLAIN return (`S f()` / `return r;`) is correct. A `string` local is
correct. It is specific to a struct-typed local with an owning field, move-returned by name.

This is why it was never seen before: the containers that use `return move result;`
(`list.copy()`, `dictionary.copy()`) own their storage through a raw `T*` field freed by a
hand-written destructor, and the raw-pointer leg is not affected - only owning FIELDS such as a
`string` are.

## Repro

```cflat
import "string.cb";
import "diagnostic/heap_audit.cb";

struct RowA { string name = default; };                 // synthesized destructor
struct RowB { string name = default; ~RowB() { } };     // user-written destructor

move RowA a1() { RowA r = default; r.name = "he" + "llo"; return move r; }
move RowB b1() { RowB r = default; r.name = "he" + "llo"; return move r; }
RowA      a2() { RowA r = default; r.name = "he" + "llo"; return r; }

extern int main()
{
    HeapAudit.enable();
    printf("base=%llu\n", HeapAudit.reportLeaks());                       // 0
    { RowA b = a1(); } printf("synth dtor + move return  =%llu\n", HeapAudit.reportLeaks());  // 1
    { RowB b = b1(); } printf("user  dtor + move return  =%llu\n", HeapAudit.reportLeaks());  // 2
    { RowA b = a2(); } printf("synth dtor + plain return =%llu\n", HeapAudit.reportLeaks());  // 2
    return 0;
}
```

`reportLeaks()` is cumulative live-allocation count: the first two blocks each leak one
`string` buffer, the plain-return block leaks none.

It does not matter how the local is produced - all three of these leak identically:

```cflat
move Row h1()      { Row r = default; r.name = "he" + "llo"; return move r; }  // built in place
move Row h2(Row v) { Row r = default; r = v.copy();          return move r; }  // assigned from copy()
move Row h3(Row v) { Row r = v.copy();                       return move r; }  // initialized from copy()
```

Unaffected (verified in the same program): `move string h4(string v)`, and
`move list<int> g4()`.

## Fix direction

Look at how a `move`-qualified return of a NAMED local transfers ownership to the caller's
receiving variable - the callee-side suppression is right, so the gap is on the caller side
(the receiving `NamedVariable` not getting `IsOwning`, or getting it only for types whose
ownership is a raw pointer rather than an owning field). Compare against the plain-return path,
which is correct, and against `string`, which is also correct.

## Workaround in use

`cflat/core/hpc/btree.cb` avoided a `move V _dupValue(V value)` helper entirely and places the
value directly into its destination slot instead (`_placeValue(node, i, V value)`), matching the
shape `list._placeAt` / `dictionary._placeValueAt` already use. No `move`-returning value helper
is needed anywhere in the container. Note `btree._dupKey` returns a PLAIN `K` (not `move K`) and
is therefore unaffected - do not "tidy" it into a move-return.
