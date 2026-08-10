# `return s` from inside `for (string s in coll)` returns a dangling borrow

Filed 2026-08-10 from the fix/forinstr round. Pre-existing on the CONTAINER leg (measured
identical on `01853aa` and fix/forinstr). On the fixed-array leg the same spelling was rc 133
plus a wrong value before that round's fix and is a wrong value only after it, so the fix
strictly improves the cell without closing it.

Sibling of `p2/alias-string-return-of-frame-local-element-dangles.md`, but a DIFFERENT spelling
and a different path: that one is `alias string f() { return dst[0]; }` - an explicit `alias`
return of an element GEP. This one is a plain (non-`alias`) return of the range-`for` LOOP
VARIABLE, which is a local alloca, so the element-read return arm
(`IsOwningArrayStringElementRead`) cannot see it.

Severity: silent wrong value at the caller (`len` correct, bytes freed).

## Repro

```cflat
import "list.cb";
string firstLong()
{
    list<string> l;                     // fixed-array spelling: string[2] dst; dst[0] = ...
    l.add("ab" + "cd");
    for (string s in l)
    {
        if (s.length() == 4) return s;  // borrow of storage this frame then destroys
    }
    return "none";
}
extern int main()
{
    string r = firstLong();
    printf("ok=%d len=%d\n", r == "abcd" ? 1 : 0, r.length());
    return 0;
}
```

Measured, scratch/fi_y_retlist.cb and scratch/fi_u_return.cb:

| spelling | 01853aa | fix/forinstr |
|---|---|---|
| `list<string>` container | `ok=0 len=4`, rc 0 | `ok=0 len=4`, rc 0 |
| `string[2]` fixed array | `r= len=4`, rc 133 | `r= len=4`, rc 0 |

The same escape happens WITHOUT a return, by moving the loop variable into a variable that
outlives the collection (scratch/rev_moveesc.cb): `string t = default; { string[2] a; ...
for (string s in a) t = move s; } t == "efgh"` is FALSE while `t.length()` is 4 - `n=12`, rc 0 on
fix/forinstr, rc 133 on `01853aa`. `move` of a cleared borrow moves the borrow, not the buffer, so
any ruling below has to cover this spelling too.

The direct element spelling is CORRECT on both binaries (scratch/fi_y_retelem.cb):
`string f() { string[2] dst; dst[0] = "ab" + "cd"; return dst[0]; }` gives `ok=1 len=4`, rc 0 -
that is exactly the arm `01853aa` installed.

## Root cause

The loop variable holds a borrow (owned bit cleared) of storage owned by the collection, and the
collection is destroyed when the function's frame unwinds. The return path classifies `s` as an
ordinary local string read; the compile-time record still says the variable owns, so nothing
deep-copies it, and at run time the cleared bit means the frame's teardown correctly does not
free it - the caller is handed `{ptr into freed storage, len}`.

## Fix direction

Either arm: (a) deep-copy at the return when the returned string is a range-`for` loop variable
whose collection is frame-local - the return arm already deep-copies the direct element read, so
this is the same ruling reached through the loop variable; or (b) reject it, matching the
direction proposed for the `alias` sibling (`PointsIntoStackFrame` on the borrowed storage).
Pick ONE ruling for both issues - they are the same program shape reached two ways - and check
the struct-field spelling (`for (W w in arr) return w.s;`) in the same pass.
