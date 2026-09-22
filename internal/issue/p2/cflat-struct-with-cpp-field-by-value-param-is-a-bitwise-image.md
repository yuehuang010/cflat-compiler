Bucket: p2 (C++ interop; latent use-after-free when the callee mutates the field; needs a ruling)

# A CFlat struct WITH a nontrivial C++ class field, passed by value to a CFlat function, is a bitwise image

Found 2026-09-21 by the by-value parameter fix (fix/cpp-byvalue-param, probe scratch/bv_holder.cb
in that worktree). Sibling of the fixed p1 (a C++ class ITSELF by value now follows the C++ rule).

```cflat
struct BvHolder { cpptw.Twin t = default; };
int take(BvHolder value) { return value.t.value(); }
BvHolder source = default; source.t.set(31); take(source);
// inside: ctor=1 copy=0 move=0 dtor=0 live=1; after source scope: dtor=1 live=0
```

Counts are balanced because the callee gets an image of the caller's bytes and never destroys it -
the native CFlat "by-value struct parameter is a borrow image" behaviour. With a C++ field that
image is unsound the moment the callee MUTATES the field: `value.s.append(...)` on a std.string
field reallocates through the image and the caller's field is left pointing at freed memory (same
mechanism as the old p1 repro 2). Not measured with std.string yet - do that first.

Needs a ruling, because the two precedents disagree:
- 8cfd0d10 made `Holder b = a;` copy-construct the C++ field (declaration = C++ rule);
- a native owning struct by-value parameter stays a borrow image (CFlat rule).
Options: (1) C++ rule - copy-construct the C++ fields into an indirect temporary, destroy once
(what the p1 fix does for the class itself); (2) keep the borrow image but reject writes through a
by-value struct parameter's C++ field; (3) reject the parameter shape, point at `Holder*`.
Recommendation: (1), by R1 and bridge transparency.
