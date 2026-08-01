# A data pointer RETURNED as a closure bypasses the provenance gate and is called

Filed 2026-07-31 by the round-1 review of `closure-param-accepts-data-pointer`.
**Pre-existing**, verified on the master binary at `4000fa1` and identical on that issue's fix
branch - not a regression.

Severity: **SILENT MISCOMPILE, then SIGBUS (exit 138).** No diagnostic. Same defect class as
the argument-side issue that was closed by `ce9858e`; only the direction differs.

## Repro

```cflat
import "function.cb";
int gq = 1;
Lambda<int(int)> give() { void* vp = &gq; return vp; }
extern int main() { Lambda<int(int)> f = give(); printf("r=%d\n", f(5)); return 0; }
```
Observed: **exit 138** on both binaries. The data pointer lands in the CODE slot of the fat
`{code, data}` closure and is then called.

## Root cause

`WidenBareOrThinToClosureFat` has THREE callers. `ce9858e` introduced a shared provenance gate
(`WidenToClosureFatChecked`, `cflat/LLVMBackend.h` ~12799) and routed two of them through it:

- `LowerByValueArg` (virtual call path) - gated
- `CreateOverloadedFunctionCall` (direct call path) - gated
- **`CoerceToFuncPtrReturn` (`cflat/LLVMBackend.h` ~9843) - NOT gated**

The RETURN path was out of scope for that fix, which was about argument passing. It widens the
same way with the same `isPointerTy()` test that, under opaque pointers, is true for every
pointer.

## Fix direction

Route `CoerceToFuncPtrReturn` through `WidenToClosureFatChecked` so all three sites share one
accept set. The gate already exists and is measured safe on 417 corpus files; this should be
close to a one-line change plus a reworded message (the current text says "closure parameter
'<name>'", which is wrong for a return - it wants a return-flavoured variant).

**Guard polarity is load-bearing: reject ONLY what you can PROVE.** The existing gate rejects
only when `TypeAndValue.Pointer` is positively set and no closure evidence exists. Do not
convert it to an allowlist - an earlier allowlist attempt in this same area false-rejected a
legal `?:` join, and `??` lowers to a load from an alloca, so value-shape walking is incomplete
by construction.

A measured fact any fix must respect: on the direct path `TypeAndValue.IsFunctionPointer` is
FALSE for every legal source of a thin `function<>` value, so gating on that flag would
false-reject all of them.

## Note on comment accuracy

While two of three sites were gated, the block comment at `cflat/LLVMBackend.h` ~12799 called
the new helper "The ONE gate" and a neighbouring comment claimed "the three sites cannot
drift". Those were corrected when this issue was filed. If this issue is fixed, the comments
become true as written again.

## Test coverage

None for the return path. `Test/errors/err_data_pointer_to_closure_param.cb` covers the
argument side on both call paths and is the natural home.

Related: [[data-pointer-into-thin-function-param-segfaults]], [[interface-issue-queue]]
(`funcptr-call-result-into-closure-param-garbage` was the ARGUMENT-side sibling of this defect;
it is fixed and its file deleted - the "fix/funcptr-callresult" landed record has the detail).
