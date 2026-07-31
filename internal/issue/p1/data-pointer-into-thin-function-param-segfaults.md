# A data pointer passed to a THIN `function<>` parameter is accepted and called

Filed 2026-07-31 as the deliberate residue of `closure-param-accepts-data-pointer` (fixed by
`ce9858e`, file deleted). **Pre-existing**, verified on the master binary at `4000fa1`.

Severity: **SILENT MISCOMPILE, then SIGSEGV (exit 139).** No diagnostic at any point. Same
defect class as the issue that was just closed - only the parameter arm differs.

## Repro

```cflat
import "function.cb";
class D { int thin(function<int(int)> f) { return f(5); } };
extern int main() { D d; int q = 1; void* vp = &q; printf("r=%d\n", d.thin(vp)); return 0; }
```
Observed on `4000fa1`: **exit 139**, no output, no diagnostic.

## Why it survived the fat-parameter fix

`ce9858e` closed the FAT (`Lambda<>`) parameter arm on both the direct and virtual paths, via a
single shared provenance gate (`WidenToClosureFatChecked`). The THIN (`function<>`) parameter
arm never widens to a fat struct, so it does not route through that gate:

- the direct path bitcasts the incoming pointer straight into the thin slot;
- the interface scorer admits it through the `arg.BaseType->isPointerTy()` clause in
  `ComputeOverloadFunction`, which is true for EVERY pointer under opaque pointers.

**Both paths agree**, so this is not an asymmetry - it is a symmetric hole, which is why it was
left rather than half-closed. Closing it on one path only would recreate exactly the
direct-vs-virtual divergence that has already caused a regression here once.

## Fix direction

Route the thin arm through the same provenance gate, so all four combinations
(direct/virtual x thin/fat) share one accept set.

**Guard polarity is load-bearing: reject ONLY what you can PROVE.** The existing gate rejects
only when `TypeAndValue.Pointer` is positively set and no closure evidence exists. That
polarity was learned the hard way here: an earlier ALLOWLIST attempt (accept only
`isa<Function>` / `IsFunctionPointer` / null) FALSE-REJECTED a legal `io.lam(k > 0 ? a : b)`,
because a `?:` join is a `select`/`phi` carrying none of the three, and `??` lowers to a LOAD
FROM AN ALLOCA rather than a join. The set of legal spellings cannot be enumerated.

A measured fact from `ce9858e` that any fix here must respect: **on the direct path
`TypeAndValue.IsFunctionPointer` is FALSE for every legal source of a thin `function<>` value**
(local variable, struct field, array element, call result, `?:`, `??`, named function). Gating
on that flag would false-reject all of them. Verify with an instrumented build before relying
on any flag, rather than reasoning about it.

Verification must include an accept-set parity table across direct AND interface calls, plus a
differential corpus sweep - argument lowering touches every call in every program.

## Test coverage

None for the thin arm. `Test/errors/err_data_pointer_to_closure_param.cb` covers the fat arm on
both paths and is the natural home for the thin legs.

Related: [[shape-mismatched-funcptr-arg-binds-silently]],
[[funcptr-call-result-into-closure-param-garbage]], [[interface-issue-queue]]
