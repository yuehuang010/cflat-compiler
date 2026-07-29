# A `Lambda<>` parameter on a DIRECT call accepts any data pointer and calls it

Filed 2026-07-29 while fixing `iface-thin-function-param-no-lowering`. PRE-EXISTING on the
direct call path, identical on `83caa7f` and on the fix branch. The VIRTUAL path had the same
hole and is now guarded; this file tracks the direct-path residue only.

Severity: SILENT MISCOMPILE, then SIGSEGV/SIGBUS. No diagnostic at any point.

## Repro

```cflat
import "function.cb";
class D { int lam(Lambda<int(int)> f) { return f(5); } };
extern int main(){ D d; int q = 1; void* vp = &q; printf("r=%d\n", d.lam(vp)); return 0; }
```

Compiles clean; exit 139 (SIGSEGV). `int*` behaves the same; `char*` (e.g. `s.data()`) gives
exit 138 (SIGBUS). Emitted IR: `insertvalue %__closure_fat_ptr undef, ptr %q, 0` - the data
pointer lands in the CODE slot of the fat closure and is then called.

## Root cause

In `CreateOverloadedFunctionCall`'s `IsFunctionPointer` branch, the fat-parameter arm widens
with `if (isa<Function>) WrapBareValueAsFatStruct else if (isPointerTy) WidenThinToFat`. Under
opaque pointers `isPointerTy()` is true for EVERY pointer, so it is not a test for "is a thin
`function<T>`" - it admits any data pointer. Overload resolution does not stop it either:
`ComputeOverloadFunction` accepts an argument for a function-pointer parameter when
`arg.BaseType->isPointerTy()`.

The virtual path is now guarded in `LowerByValueArg`, which REJECTS only what it can prove is
a data pointer (`ArgumentIsProvablyDataPointer`: not an `llvm::Function`, not a null constant,
not `TypeAndValue.IsFunctionPointer`, and `TypeAndValue.Pointer` positively set) and widens
everything else, reporting
`cannot pass ... to closure parameter '<name>': ... a data pointer would be called as code.`

The polarity is load-bearing and was learned the hard way. The first cut was an ALLOWLIST -
accept only `isa<Function>`, `IsFunctionPointer` or null, reject the rest - and it false-rejected
a legal `io.lam(k > 0 ? a : b)`, because a `?:` join is a `select`/`phi` that carries none of
the three. Every gap in an allowlist is a false rejection, and the set of legal spellings cannot
be enumerated (`??` lowers to a LOAD FROM AN ALLOCA, not a join, so even a value-shape walker
would have been incomplete on day one). Reject-only-when-proven puts unknown shapes on the
accept side by construction; the worst case is a missed diagnostic, which is merely the
pre-existing behaviour.

## Related: a null closure that is invoked

`d.lam(nullptr)` is accepted on BOTH paths (it widens to `{null, null}`), which is fine while
the callee only stores or forwards it - but a callee that INVOKES it jumps to address 0 (exit
139 on the direct path today). Same defect class as the data-pointer jump above: a closure
whose code slot is not code. Whatever guards one should consider the other; rejecting `nullptr`
here would need the same accept-set discussion, since it is legal today on both paths.

## Fix direction

Apply that same provenance gate at the direct site. Two constraints on any such work:

- **The accept set must stay IDENTICAL to the direct path.** Diagnostics may be better on one
  arm; what is accepted may not differ, or the same program compiles through one call spelling
  and not the other. The round-3 regression was exactly this asymmetry.
- **The durable fix is frontend-recorded provenance, not LLVM value interrogation.** Interrogating
  the `llvm::Value` cannot answer "was this a closure in the source" once it has been through a
  join, an alloca round-trip, or a container. The codebase already solved the same
  provenance-at-a-join question for interface fat pointers with a value-keyed ledger stamped at
  the join (`PropagateFatInterfaceJoin`, `RegisterFatInterfaceValueTypeName`) - that is the
  shape to copy, extended to `?:`, `??`, if/else joins, and containers (`list<function<>>`,
  `Box<function<>>`). Build it once and drive BOTH paths from it, rather than per-arm. The reason it was NOT done in the change that added it
virtually: on the direct path `arg.TypeAndValue.IsFunctionPointer` is not
known to be set for every legal source of a thin `function<>` value (struct field, array
element, generic substitution, core-library call result), so gating there risks FALSE
REJECTIONS of working code. Before flipping it, enumerate those sources and confirm each sets
the flag - a green `test.sh` is not sufficient evidence, since no in-repo `.cb` exercises most
of them. `Test/errors/err_data_pointer_to_closure_param.cb` covers the virtual side and is the
model for the direct-side test.
