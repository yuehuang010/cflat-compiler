# A range-`for` loop variable reads freed bytes when the body overwrites its own slot

Filed 2026-08-10 from the fix/forinstr review round. Pre-existing in substance: the loop
variable was always a bit-copy of the element's `{ptr,len}`, so the pointer was already stale
after a same-slot store on `01853aa`. That round's fix (clearing the owned bits) removed the
teardown double free, which is what previously MASKED this cell behind an rc 133 crash.

Severity: silent wrong value inside the loop body. No crash, no leak, correct-looking `length()`.

## Repro

```cflat
extern int main()
{
    string[2] dst;
    dst[0] = "ab" + "cd";
    dst[1] = "ef" + "gh";
    int i = 0;
    int n = 0;
    for (string s in dst)
    {
        dst[i] = "zz" + "yy";                             // drops (frees) the old buffer
        n += s.length();                                  // 4 - length survives
        n += s == "abcd" || s == "efgh" ? 1000 : 0;       // never fires: bytes are freed
        n += s == "zzyy" ? 100000 : 0;                    // never fires: s is not rebound
        i = i + 1;
    }
    n += dst[0] == "zzyy" ? 10 : 0;
    n += dst[1] == "zzyy" ? 20 : 0;
    printf("n=%d\n", n);
    return 0;
}
```

Measured (scratch/rev_uaf.cb, scratch/rev_uafctl.cb):

| binary | result |
|---|---|
| `01853aa` | rc 133, no output |
| fix/forinstr | `n=38`, rc 0 - i.e. `8 + 10 + 20`; `s` matches NEITHER the old nor the new value |

Control with the store removed (scratch/rev_uafctl.cb) gives `n=2008` on both binaries, so the
comparison legs are wired correctly and the `38` is a genuine read of freed memory.

## Root cause

The loop variable is now a cleared BORROW of `dst[i]`'s `{ptr,len}`, taken once at the top of the
iteration. A store into the same slot goes through the drop-old path and frees the buffer the
borrow points at. Nothing invalidates or rebinds the loop variable, so the rest of the body reads
freed heap.

The existing regression coverage does not catch this: `Test/test_move.cb`
`forin_write_through_sum` and `scratch/fi_m_mutate.cb` both read only `s.length()` after the
store, and the length field is intact in the stale copy.

## Fix direction

This is the range-`for` instance of a general "borrow invalidated by a write through the owner"
question, so pick a ruling that also covers `alias string a = dst[0]; dst[0] = ...;`:

1. Reject a store into the collection while a range-`for` over it is live (a located error at the
   store naming the loop variable). Needs the loop to record its collection's storage so the
   assignment path can compare - and note `forin_write_through_*` in `Test/test_move.cb` and
   `scratch/fi_m_mutate.cb` are exactly this shape, so they would need re-spelling.
2. Re-load the loop variable after any store that could alias the collection - defensive, costly,
   and still wrong for a store through an aliasing pointer.
3. Accept it as a documented borrow hazard and add a value-discriminating regression that PINS the
   current behaviour, so a later ownership change cannot silently move it.

Whichever is chosen, extend the coverage to compare the loop variable's BYTES (not just its
length) after the store - that is the assertion that was missing.
