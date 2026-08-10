# Reading an OWNING STRUCT element out of a raw `T*` heap array double-frees it

Filed 2026-08-10 by `fix/rawheap`, the STRUCT twin of the `string` issue that branch fixed. The
`string` half is closed (a raw `new string[n]` element now borrows in both directions); this half
does not share its fix, because a struct has no runtime owned bit to clear.

Severity: double free (abort, rc 133).

## Repro

```cflat
int dtorCount = 0;
struct Node { int v = default; ~Node() { dtorCount++; } };
struct Box { unique Node* p = default; };
Box mk(int v) { Box b = default; b.p = new Node(); b.p->v = v; return b; }
extern int main()                       // scratch/rh_13d
{
    {
        Box b = mk(7);
        Box* h = new Box[2];
        h[0] = b;                       // Part 6 owning-value arm: MOVES, `b` is nulled
        Box q = h[0];                   // borrows the slot
        printf("q=%d\n", q.p == nullptr ? -1 : q.p->v);
    }                                   // prints q=7, then rc 133
    printf("dtor=%d\n", dtorCount);
    return 0;
}
```

Measured on `b220d54` and on `fix/rawheap` alike: `q=7`, then rc 133.

## Root cause (measured, not inferred)

Two destructions of one pointee. `Box* h = new Box[2]` is never deleted, so the slot itself is not
the second owner; the second destruction comes from the element READ materialising a destructible
temporary. `scratch/rh_13e` proves it in isolation: with NO local bound at all, a bare
`printf("%d", h[0].p->v)` after the store already reports `dtor=1` - the statement-end flush
destructs the read temp and leaves the slot dangling. Add `Box q = h[0];` and `q`'s scope-exit
destructor frees the same pointee again.

## Why the `string` fix does not generalise

`string` carries a RUNTIME owned bit, so `fix/rawheap` could make the raw-heap slot and the read
borrow by masking that bit off (`ClearStringOwnedBit`). A struct has no such bit: a local
`Box q` runs its full destructor at scope exit unconditionally, so a "borrowing" read cannot be
expressed the same way. Any fix here has to change what the READ produces (consume the slot, or
copy), and the single-index-GEP read shape is shared with container internals - the plan's
LOAD-BEARING INVARIANT (`T tmp = _data[i]` in list `_partition`, the dictionary rehash) - so the
admission has to key on the same alloca-backed-local-`T*` provenance the `string` arms now use
(`IsRawHeapStringElementRead`), not on the GEP shape.
