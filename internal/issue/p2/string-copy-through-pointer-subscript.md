# p2: global and implicit-this pointer subscript string ownership

## Global pointer bases

Global `string*` subscripts such as `g[0]` remain unresolved. The four failing review shapes
are c47 (simple global), p01 (an uncalled raw-new setter changes the runtime value), p06 (a
selected address), and p14 (a callee rebinds the global); each double-frees on master and r3.
The r2 compile-time global provenance flag depended on source emission order, not runtime
state. Copying every global subscript would also leak in raw-new cases p02 and p04 and changed
allocation/free counts for p08, so a runtime provenance design or a ruling is still needed.

## Implicit-this field base

Inside a method with `string* gq`, both `string t = gq[0];` and
`string t = this->gq[0];` still double-free (p13a). R1 made three attempts to extend the local
pointer-slot proof: resolve the field through `FindImplicitThisField`, prove its normal
struct-index GEP, and handle C++ offset-layout fields as `i8` byte-offset GEPs using
`CxxFieldOffsets` (including the folded field-zero address). The byte-offset form is the actual
C++ field address shape, but even that proof did not select the copy path; the two spellings
remain out of scope after three attempts.

## T413 residual: raw-new `(*pp)[i]` that cannot be proven copies and leaks

T413 round 2 adopts `(*pp)[i]` through a single local referred slot only when every value that
slot can hold is a raw `new` result or null: direct stores, stores through `*pp`, defined callees
receiving `pp` / `&slot` (body scanned: stores through the parameter only), and, inside a loop,
every later use on the parse tree. Anything else copies. That is sound against double free, but a
raw-new array read through an unproven shape now leaks the element (raw `delete[_]` never runs
string dtors): callee defined after the reading function or forwarding the pointer on, a slot
that holds raw-new on one path and a borrowed address on another, a loop that passes `pp` /
`&slot` to a call. Needs the same runtime provenance design as the global case.
