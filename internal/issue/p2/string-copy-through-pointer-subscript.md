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

## T392 leftovers: `(*pp)[i]` shapes the local proof does not cover

T392 copies a borrowed element for `(*pp)[i]` only when `pp` is a plain local with exactly one store of a local or parameter slot and
no store through `*pp`. Found by the T392 review (2026-10-21); these still double-free on master and on T392:
- `pp` is a parameter (`scratch/repro_keep/box1021/t392_rev/rev_t392_opus_c12_pp_param.cb`)
- `pp` has two possible stores (branch-dependent target)
- a global `string** gpp`
- a `string** pp` struct field
A store through `*pp` (`*pp = new string[2];`, `rev_t392_opus_c20_rebind_through_pp.cb`) falls back to master behavior on purpose.
The T392 proof also falls back to master behavior (shallow adopt) when the read is inside any loop (a later store in the body is not
visible to the single pass) or when pp reaches a call or is stored elsewhere (a callee may rebind `*pp`); a borrowed `(*pp)[i]` read in
those positions still double-frees as on master.
