# `span<T>` element access with an OWNING element type aliases the element, and `set` drops the write

Filed 2026-08-10 by the review of `fix/viewelem`, which measured these while auditing the span
fast path that round deliberately left alone.

Severity: double free (abort, rc 133) AND a silently lost write (wrong answer, rc 0 at the print).

## Repro

All measured IDENTICAL on `0cfd9f7` and on `fix/viewelem` - this is not a regression of that round,
it is the span-shaped hole its `T[]`-view fix does not reach.

### (a) `span<T>.get` hands back a second owner - `scratch/ve_sp1`, `scratch/ve_sp2`

```cflat
import "span.cb";
extern int main()
{
    string[2] base; base[0] = "ab" + "cd";
    string[] v = base;
    span<string> s; s.wrap(v, 2);
    string q = s.get(0);
    printf("q=%d shared=%d\n", q == "abcd" ? 1 : 0, q.data() == base[0].data() ? 1 : 0);
    return 0;                       // prints `q=1 shared=1`, then rc 133
}
```

The owning-struct spelling (`span<Box>` over `Box { unique Res* item; }`, `Box q = s.get(0)`) is
rc 133 the same way.

The discriminator is that the SAME shape hand-written is CORRECT after `fix/viewelem`:

```cflat
struct SSpan { string[] _p = default; void wrap(string[] p) { _p = p; }
  string get(i64 i) { return _p[i]; }
  void set(i64 i, string v) { _p[i] = v; } };
// ... s.get(0) -> shared=0, rc 0.  The generic `GSpan<T>` spelling is equally correct.
```

So it is not the field-view provenance and not genericity - it is the fast path.

### (b) `span<string>.set` of an OWNED temp loses the write, then aborts

```cflat
import "span.cb";
extern int main()
{
    string[2] sb; sb[0] = "aa"; sb[1] = "bb";
    string[] sv = sb;
    span<string> ss; ss.wrap(sv, 2);
    ss.set(1, "ij" + "kl");
    printf("sb1=%d\n", sb[1] == "ijkl" ? 1 : 0);
    return 0;                       // prints `sb1=0` (write LOST), then rc 133
}
```

A string LITERAL argument (`ss.set(1, "zz")`) writes through correctly, and the plain view spelling
`sv[1] = "ij" + "kl"` writes through correctly - only the owned-temp argument through the fast path
is dropped. `span<int>.set` is correct.

## Root cause

`MainListener_PostfixExpression.cpp` (the `ArrayViewBufferFieldIndex` fast path around the
`LowerSpanElementAccess` call) intercepts `get`/`set` on an lvalue span and lowers them to a raw
load/store through the buffer element GEP, so the real method body never runs. That bypass reaches
NONE of the ownership arms:

- `get` builds a `namedVar` with `Storage = nullptr` and `Primary = LoadNamedVariable(elem)`, so
  `IsOwningArrayStringElementRead` (which requires `nv.Storage` to be a GEP) cannot fire and the
  `{ptr,len,owned}` triple is bit-copied to a second owner.
- `set` goes straight to `CreateAssignment(valValue, elem.Storage, ...)`, skipping the assignment
  path's owning-source handling entirely - which is why an owned temp's buffer is neither
  transferred nor deep-copied, and the destination keeps its old bits.

`fix/viewelem` recorded `elem.IsViewElement = true` in `LowerSpanElementAccess` so a future consumer
inherits the right provenance, but nothing on this path reads it yet.

## Fix direction

The fast path exists purely to attach the noalias scope metadata (`AttachViewNoalias`) that the
`vectorize` contract depends on. Restrict it to element types that own nothing (POD / primitive),
and let an owning element type fall through to the real `span<T>` method body - which, after
`fix/viewelem`, is already correct (the hand-written `GSpan<T>` above is the proof). That keeps the
HPC path byte-identical (its element types are `double`/`i64`/POD) while removing the double free.
The alternative - reproducing the ownership arms inside the fast path - duplicates the logic the
view-element arms already implement and is not worth it.

Guard the fix with the two probes above plus a `span<double>` vectorize benchmark, to confirm the
noalias metadata is still emitted for the POD case.
