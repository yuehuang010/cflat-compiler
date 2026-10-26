# p3: Borrowed view fields can outlive their aggregate through a call

## Left after T472 round 3

T472 treats a direct `S[]` argument as borrowing, matching the rule for an `S[]` local. The callee's retention behavior is not
checked, so a callee may keep the view after the local aggregate releases its field at scope exit.

## Repros

- `scratch/rev_t472_r2/e8b_view_param_store.cb`: `keepV(a.hs[0].f)` stores the view globally. Master: `L=1 D=0 V=7`, ASAN clean. T472 r3: `L=0 D=0 V=3`, ASAN heap-use-after-free. Its local twin `e8L_byval_param_store_local.cb` is ASAN heap-use-after-free on master.
- `scratch/rev_t472_r2/e12_return_via_call.cb`: `idv(a.hs[0].f)` returns the borrowed view. Master: `L=1 D=0 V=12`, ASAN clean. T472 r3: `L=0 D=0 V=4467`, ASAN heap-use-after-free. Its local twin `e12L_return_via_call_local.cb` is ASAN heap-use-after-free on master.
- `e5_elem_ptr.cb` and `e8_byval_param_store.cb` are ASAN-clean on master and T472 r3. Their `S[]` local twins `e5L_elem_ptr_local.cb` and `e8L_byval_param_store_local.cb` are ASAN heap-use-after-free on master.

The e8b/e12 behavior is explicitly exempted by the T472 round-3 acceptance rule: direct borrowed arguments have the same lifetime boundary as `S[]` locals. The master field cases happen to leak and remain readable, while the local twins demonstrate the existing release/UAF behavior.

## Root cause

The compiler does not summarize whether a borrowing callee stores or returns its argument. T472 leaves the own bit armed for a direct borrowing argument, and the aggregate's scope-exit cleanup releases the backing block.

## Fix direction

Add a cached per-parameter retention summary or reject passing a possibly retained field view across a function boundary. Do not infer non-retention from the borrowing parameter annotation alone.
