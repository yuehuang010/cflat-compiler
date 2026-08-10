# A funcptr parameter DECLARED `move` does not transfer ownership at the indirect call

Filed 2026-08-09 from the fix/lamsink review (round-1 probe `rev_a5_declared_move`), re-measured
on master `43ccb90`. Pre-existing: identical before and after the lamsink fix, on both binaries.

Severity: double free (abort, rc 133).

## Repro

```cflat
import "function.cb";
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }
void sink(move UBox p) { printf("got=%d\n", p.item->id); }
extern int main() {
    UBox a = umk(5);
    function<void(move UBox)> f = sink;
    f(a);
    printf("dtor=%d\n", dtor);
    return 0;
}
```

Measured (`scratch/dmfp_indirect.cb`): prints `got=5`, `dtor=1`, then aborts on `a`'s teardown -
rc 133. The direct-call oracle (`scratch/dmfp_direct.cb`, `sink(a);`) is rc 0 with `dtor=1`:
the callee frees, the caller's slot is transferred and not freed again.

## Root cause

The indirect call site (`MainListener_PostfixExpression.cpp`, the closure/funcptr call path) only
runs caller-side transfer for INFERRED sinks: `ApplyFuncPtrSinkTransfer` early-outs unless some
`FuncPtrParam` carries `IsOwningSink`, and it deliberately synthesizes its `TypeAndValue` params
with `IsMove = false`. A parameter declared `move` in the funcptr TYPE spelling sets
`FuncPtrParam::IsMove` (the per-param agreement check on funcptr assignment reads it), but nothing
at the indirect call maps `IsMove` to the `ApplyMoveParamTransfer` treatment a direct call gets.
So the callee consumes (its signature is a real `move` param) while the caller still owns - both
free.

## Fix direction

At the indirect call site, treat a `FuncPtrParam` with `IsMove` exactly like one with
`IsOwningSink`: include it in the `ApplyFuncPtrSinkTransfer` early-out predicate and set
`IsMove = true` on the synthesized `TypeAndValue` for that parameter so `ApplyMoveParamTransfer`
runs its normal declared-move path (transfer + MarkVariableMoved + use-after-move diagnostics).
Accept-set care: the inferred-sink path must keep `IsMove = false` (that asymmetry is deliberate -
see the fix/lamsink digest entry); only a param whose TYPE spells `move` should take the declared
path. A lambda literal assigned into a `Lambda<void(move UBox)>` destination is currently REJECTED
by the per-param agreement check (pinned leg e1 of the lamsink corpus), so only named functions
and matching-spelling lambdas can reach this today.

## Related

`internal/issue/p1/inferred-owning-sink-is-lost-when-a-closure-crosses-a-declared-boundary.md` -
the inferred-flag twin of this declared-flag gap; a shared fix at the indirect call site likely
covers the transfer half of both.
