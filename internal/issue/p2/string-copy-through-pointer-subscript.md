# p2: double-pointer subscript string copy shallow-adopts (double free)

## Repro (ASan double-free)

```cflat
string source = "abcdefghijklmnopqrstuvwxyz0123456789abcdefghij".copy();
string* ps = &source;
string** pp = &ps;
string t = (*pp)[0];
```

The standalone repro is `scratch/t359_tmp/post/probes/c63_ptrptr_idx.cb`. The expected
value is 46; master and the r1 build both compile, then fail at runtime with ASan rc 134.

## T359 r3 scope-cut findings

Global pointer subscripts remain out of scope. The r2 global provenance set was updated while
functions were emitted, so it followed source-compilation order rather than runtime order.
The global base `g[0]` double-frees on master and r2 in c47, p01 (runtime `g = &src` after
an uncalled raw-new setter), p06 (the selected `&src` branch), and p14 (a callee rebinds `g`).
The r2 gate fixed other global spellings, but that compile-time flag cannot prove which pointer
is in the global slot at the read.

A sound fix needs runtime provenance through calls and branch joins, or a ruling to copy an
unknown global subscript. The latter costs a leak for raw-new globals: p02 (reader emitted before
the setter) and p04 (pointer arithmetic clears provenance) were clean on master but leaked on r2.
The r2 `GetGlobalVariableNV` inheritance also made p08, a local alias of a raw global, allocate
and free one fewer string than master. R3 removes the global provenance state; p01, p02, p04, p05,
p06, p08, p09, p14, p16, p17, p18, and c47 now match master in the review probe.

Implicit-this field `gq[0]` in p13a is another unresolved shape: its base is a this-field GEP,
not a named pointer slot. It still double-frees on master and r3. These global and implicit-this
shapes are not covered by the local pointer-slot proof in T359.

## R1 root-cause notes

R1 fixed the direct named-pointer cases by identifying a one-level subscript base and its
declared pointer binding. Three attempts still left c63 at ASan rc 134. The saved r1 report
does not preserve separate patch notes for each attempt; the final helper and repro narrow
the remaining gap to `(*pp)`: it adds a dereference before the subscript, so the helper's
direct pointer-slot proof does not establish the element's owner. c63 remains out of scope
for T359 r2; do not widen that proof as part of the current fix.

## Fix direction

Trace a subscript through the unary dereference to the pointer value loaded from `pp`, then
carry enough ownership provenance to decide whether the selected string is a borrow that
must be copied. Add a value and HeapAudit leg before changing this path.
