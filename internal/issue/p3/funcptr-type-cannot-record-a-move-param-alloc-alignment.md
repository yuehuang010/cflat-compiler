# A funcptr TYPE cannot record a `move` parameter's `alignas` clause

Filed 2026-08-10 from the `fix/fpmove` review (probe `scratch/rev_j2_alignptr_named`).

Severity: missing check (an over-aligned block can reach a `move` funcptr parameter whose callee
does NOT declare the matching clause, and the callee then frees it with the wrong alignment).
No false rejection - the review amend suppressed that half.

## Repro

```cflat
import "function.cb";
void take(move alignas(0, 64) int* p) { printf("t=%d\n", p[0]); delete[] p; }
extern int main() {
    alignas(0, 64) int* b = new int[4] alignas(0, 64);
    function<void(move int*)> f = take;   // the TYPE cannot spell `alignas`
    f(b);
    return 0;
}
```

Accepted. So is the direct oracle `take(b);`. But swap the callee for one WITHOUT the clause and
the direct call is rejected ("cannot move the over-aligned buffer ... that alignment is a property
of the allocation, not of the type") while the indirect call still compiles.

Four-cell measurement (`scratch/vr_a1`/`vr_a2`/`vr_b1`/`vr_b2`, `--run`, macOS arm64 Release),
pre = `d1b95fe`, post = `fix/fpmove`:

| Cell | callee clause | call | pre | post |
|------|---------------|------|-----|------|
| `vr_a1_match_direct` | `alignas(0,64)` | direct | rc 0 | rc 0 |
| `vr_a2_match_indirect` | `alignas(0,64)` | indirect | rc 0 | rc 0 |
| `vr_b1_mismatch_direct` | none | direct | rejected | rejected |
| `vr_b2_mismatch_indirect` | none | indirect | rc 0 | rc 0 |

So the indirect call NEVER caught the mismatched callee, on either binary: this is a pre-existing
hole that `fix/fpmove` neither opened nor closed. What the round did do was briefly turn `vr_a2`
into a rejection with no accepting spelling (measured on a binary built with the amend reverted:
both `vr_a2` and `vr_b2` rc 1, "cannot move the over-aligned buffer"), which is the accept-set
regression `paramsCarryAllocAlign = false` removes.

## Root cause

`TypeAndValue::FuncPtrParam` (`cflat/LLVMBackend.h:655`) has no `AllocAlignValue` member, and the
grammar has no funcptr-type spelling for it - `function<void(move alignas(0, 64) int*)>` is an
ANTLR parse error ("mismatched input 'alignas' expecting ')'"), as is a bare `double[]` element.
`FuncPtrParamAsTypeAndValue` (`cflat/LLVMBackend_WinRT.cpp:1539`) therefore synthesizes
`AllocAlignValue = 0`, which the alignment gate in `ApplyMoveParamTransfer` would read as
"parameter declares no alignment" and reject every over-aligned argument. `fix/fpmove` passes
`paramsCarryAllocAlign = false` from `ApplyFuncPtrSinkTransfer` so the gate is skipped rather than
decided on a value the type never carried - restoring the base binary's accept set on this axis.
It costs no coverage: per the table above the indirect call never caught the mismatched callee.

**The hazard is Windows-only at run time, which is why no macOS suite can catch it.** The two
deallocators really do differ in the IR - a callee WITH the clause emits
`call void @___delete_aligned_void_U8Ptr_`, one WITHOUT emits `call void @"_operator delete..."` -
but on POSIX `_aligned_free` is defined as plain `free` (`core/cruntime.cb`, the `else` arm of the
`if const (__WINDOWS__)` pair) and `operator delete` ends in `free` too, so the mismatch is a
no-op here and `vr_b2` runs clean. On Windows the MSVC CRT `_aligned_malloc`/`_aligned_free` pair
is genuinely distinct and releasing an over-aligned block through plain `free` corrupts the heap.
Any regression leg for this must therefore assert a DIAGNOSTIC, not a run-time result.

## Fix direction

Add `uint64_t AllocAlignValue` to `FuncPtrParam`, populate it in `FuncPtrSigOfSymbol` /
`FuncPtrSigOfBoundFunction` from the named function's declared clause, round-trip it in BOTH
serializers (`LLVMBackend.cpp` key `aav` next to `mv`, and `TvToJson`/`TvFromJson` in
`LLVMBackend_StateAndImports.cpp` - the `--init` rule), include it in the per-param agreement
check on funcptr assignment, and drop the `paramsCarryAllocAlign` escape hatch. A grammar
spelling for `alignas` inside a funcptr type is optional: binding a named function already
supplies the value, and a lambda literal cannot own an over-aligned block.
