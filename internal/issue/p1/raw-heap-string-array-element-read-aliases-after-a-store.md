# A `string` element read out of a raw `string*` heap array aliases the element once it is stored

Filed 2026-08-10 by `fix/viewelem`, which measured it while freezing the accept-set leg the
`array-view-string-element-read-aliases-the-element` file asserted.

Severity: double free (abort, rc 133).

## What the deleted issue file claimed, and what is actually true

`array-view-string-element-read-aliases-the-element.md` froze this exclusion:

> A raw heap array (`string* h = new string[2]; string q = h[0];`) is deliberately NOT in this
> family ... measured rc 0 with a shared buffer

That measurement is real but it is taken on a slot that was never stored into - an empty,
non-owning `{ptr,len}` pair, which no one frees. Store an OWNED string into the slot first and the
same read aborts:

```cflat
extern int main()                       // scratch/ve_r05
{
    string* h = new string[2];
    h[0] = "ab" + "cd";
    string q = h[0];
    printf("q=%d same=%d\n", q == "abcd" ? 1 : 0, q.data() == h[0].data() ? 1 : 0);
    return 0;                           // prints `q=1 same=1`, then rc 133
}
```

```cflat
extern int main()                       // scratch/ve_r05b - named-local source
{
    string a = "ab" + "cd";
    string* h = new string[2];
    h[0] = a;                           // deep-copies: the SLOT now owns a buffer (same=0)
    string q = h[0];
    return 0;                           // rc 133
}
```

Both are rc 133 on `0cfd9f7` and on `fix/viewelem`. The store alone is clean (`scratch/ve_r05e`,
rc 0, `same=0`), so - exactly as in the view issue - the defect is the READ.

The never-stored spelling really is rc 0 on both binaries and is frozen as the accept leg
`avelr_rawheap_*` in `Test/test_move.cb`.

## Root cause

Part 6's owning slot arm (`MainListener_Expressions.cpp`, `destIsElemSlot`) deep-copies a named
`string` source into ANY single-index GEP slot, so `h[0] = a` makes the raw heap slot a real owner.
The READ side has no matching arm: `IsOwningArrayStringElementRead` admits a two-index fixed-array
GEP and a single-index GEP carrying `NamedVariable::IsViewElement`, and a raw `string*` carries
neither, so the read stays a shallow borrow and both the local and the slot free the buffer.

## Fix direction

The two halves disagree and one of them has to move. Either the STORE into a raw `T*` element stops
taking ownership (matching `new string[n]`'s own `delete` diagnostic, which says the allocation does
not own assigned strings - then the read's borrow is correct and nothing else changes), or the READ
deep-copies whenever the slot may own (then `T*` behaves like `T[]` and the two spellings converge).
The second is the bigger behaviour change: a raw `T*` is also how container-internal buffers are
spelled, so any widening there has to be checked against the plan's LOAD-BEARING INVARIANT (list
`sort`/`_partition` and dictionary rehash bit-shuffles) - which is exactly why `fix/viewelem` used
the view provenance rather than the GEP shape.
