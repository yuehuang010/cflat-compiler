# Reading a `string` element out of an array VIEW aliases the element's buffer

## Summary

`string q = v[0];` over a `string[] v` shallow-copies the element's `{ptr,len}` pair, including
the runtime owned bit. Both the local and the underlying buffer's real owner free it: rc 133.

This is the VIEW sibling of the fixed-array read, fixed on the fixed-array side by the
two-index-GEP arm (`IsFixedArrayStringElementRead`). A view subscript is a SINGLE-index GEP,
which that arm deliberately excludes - the single-index container-slot gate is load-bearing for
`list`/`dictionary` element access, whose `get` already hands back a cleared-owned-bit borrow.

## Repro

Both spellings are rc 133 on bfb5943 AND after the fixed-array read fix (measured):

```cflat
extern int main()
{
    string a = "ab" + "cd";
    string[] v = { a, "b" };
    string q = v[0];                 // shallow: q and the buffer's owner both free it
    printf("q=%d\n", q == "abcd" ? 1 : 0);
    return 0;                        // rc 133
}
```

```cflat
extern int main()
{
    string[2] dst;
    dst[0] = "ab" + "cd";
    string[] v = dst;                // view over a fixed array
    string q = v[0];
    return 0;                        // rc 133
}
```

The brace-init alone is clean (`string[] v = { a, "b" }; printf(v[0] == "abcd")` is rc 0), so the
defect is the READ, not the view construction.

## Root cause

A view element read is a single-index GEP over `%string`, indistinguishable at that shape from a
container's internal slot access. The fixed-array read arm keys on a TWO-index GEP over an array
type precisely to stay out of the container gate, so it cannot cover this.

## Fix direction

The discriminator has to be the view's own `TypeAndValue.IsArrayView` / `NoaliasScopeId`
provenance on the receiver, not the GEP shape - a view is a user-visible borrow of storage the
view does not own, whereas `list._data[i]` is container-internal. Verify against
`internal/issue/p1/array-view-element-store-orphans-the-old-element.md`: the store side of the
same receiver is already known broken, and both halves probably want one ruling.

A raw heap array (`string* h = new string[2]; string q = h[0];`) is deliberately NOT in this
family: `new string[n]` does not take ownership of assigned strings (see the `delete` diagnostic),
so its element read must stay a borrow - measured rc 0 with a shared buffer, and deep-copying
there would orphan the slot's buffer instead.
