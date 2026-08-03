# A funcptr candidate refuted on its SIGNATURE rebinds onto a non-`void*` pointer sibling

Filed 2026-08-03, split out of `funcptr-overload-binding-ignores-signature` when that file's items
2 and 3 were closed by `fix/funcptr-close` and it was deleted (its content lives in the
`fix/funcptr-close` landed record in [[interface-issue-queue]]). This is the residue of item 2:
the `void*` half is closed, this half is not.

Severity: **P1 - memory-unsafe, and silent in the cheaper shapes.** A function-pointer VALUE is
handed to a callee as a data pointer and written through.

## Repro - a struct-pointer sibling: SIGBUS, no diagnostic

`scratch/fp2-structptr.cb`:

```cflat
import "function.cb";
struct Rec { int a; int b; };
double ro3(double x) { return x + 1000.0; }
int lam3(function<int(int)> f) { return f(5); }
int lam3(Rec* r)               { r->a = 11; r->b = 22; return r->a + r->b; }
extern int main() { function<double(double)> w = ro3; printf("d=%d\n", lam3(w)); return 0; }
```

`exit 138`, no output, no diagnostic. The `function<int(int)>` candidate is refuted on its
signature, the `Rec*` sibling absorbs the value, and `r->a = 11` writes through the code pointer.

The cheaper shape is silent rather than fatal - `scratch/fp2-intptr.cb`, same construction with
`int lam2(int* p) { return 888; }`, prints `c=888` and exits 0 because the callee never
dereferences.

**Pre-existing, not a regression.** `fix/funcptr-close` gates only on
`candidateParamItr->TypeName == "void"`, so nothing on this path changed; both repros behave
identically before and after it.

## Root cause

Same as the `void*` half, one parameter type over. `result = -1` in `ComputeOverloadFunction`
(`cflat/LLVMBackend.h`) is a PREFERENCE verdict - "this candidate does not match" - not a
validation one. Once `FuncPtrSignaturesProvablyDiffer` refutes the function-pointer candidate, the
scorer simply moves on, and under opaque pointers `CompareUpconvert` sees the argument and a
`Rec*` parameter as two indistinguishable `ptr`s and accepts.

The `void*` half was closed by refusing the conversion at the argument: a function-pointer or
closure VALUE is code, not data (`ArgumentIsFunctionPointerish` + the `FunctionPointerShapeOf(...)
== 0` shape gate, so `function<T>*` and `function<T>[N]` - which really are data pointers - keep
converting). That gate is keyed on `TypeName == "void"` and does not reach any other pointee.

## Fix direction

Widen the same gate from `void*` to ANY pointer parameter whose pointee is not itself a
function-pointer type. The pieces already exist - `ArgumentIsFunctionPointerish` and the shape
check are both in place and both already carry the must-keep-binding cases.

Two things to prove before widening, neither measured yet:

- **C interop.** A `void*`-typed callback slot is the common spelling, but a header-bound
  parameter may arrive as some other pointee (`Rec*`, `char*`) and legitimately receive a function
  address. Measure `core/*.cb` and the header-import paths before assuming the gate is free.
- **The `function<T>` parameter arm.** `ComputeOverloadFunction`'s funcptr clause accepts an
  argument when `arg.BaseType->isPointerTy()` - the mirror direction, a raw data pointer into a
  function-pointer slot. That leg is a separate filed issue
  ([[data-pointer-into-thin-function-param-segfaults]] was its `void*` case) and widening this gate
  must not be read as closing it.

Do NOT approach this by turning the scorer's `-1` into a hard error. That was considered and
rejected: `-1` is genuinely per-candidate, and a legal call whose FIRST candidate is refuted must
still bind a later matching one - pinned by "matching signature beats the void* sibling" in
`Test/test_function_ptr.cb`.

## Coverage that already exists and must keep passing

- `Test/test_function_ptr.cb`: "data pointer still binds the void* sibling", "address of a funcptr
  slot binds void*", "array of funcptrs binds void*" - the three shapes a widened gate breaks
  first. The last two are programs master compiles and runs; a first cut of the `void*` gate keyed
  on "carries function evidence" and rejected both.
- `Test/errors/err_data_pointer_to_closure_param.cb`: the `rebindReject` leg (the `void*` half,
  closed). A widening belongs next to it, same two-same-arity-overloads construction so the
  overload SCORER is what decides.

Related: [[shape-mismatched-funcptr-arg-binds-silently]],
[[data-pointer-returned-as-closure-not-gated]], [[interface-issue-queue]]
