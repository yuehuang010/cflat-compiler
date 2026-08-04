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

## PARKED 2026-08-03 - branch `fix/funcptr-rebind`, worktree `../cflat-fix-funcptr-rebind`

Two review rounds, stopped before a third at the maintainer's instruction. The branch is ONE
commit `b2e0e9b` on `904f026`; nothing merged, `master` untouched. The work is close - the fix
direction above is confirmed correct and the bar is green (`test.sh` 576/0/8,
`example_mac.sh` 35/0, A/B `--check` sweep over 546 files = 1 intended difference) - it is parked
on two open review findings, not on a wrong approach.

**What the branch already does, all measured pre/post and reviewed clean:**

- Widens the gate from `TypeName == "void"` to any pointer parameter, via two new helpers
  `ArgumentIsCodeValue` / `ParameterStoresData` (`cflat/LLVMBackend.h` ~17669).
- Gates the implicit `char*` -> `string` coercion the same way - `string` is not a pointer but is
  reached by that coercion, and it lowered to `operator string(char*)` reading the callee's machine
  code (proven from `--no-opt` IR).
- Runs the gate on the VARIADIC short-circuit (~17700), which round 1 found bypasses per-argument
  scoring entirely: `lam(Rec*, ...)` absorbed the code value exactly as the non-variadic sibling
  did, exit 138 with no diagnostic. Only DECLARED parameters are judged - an argument in the `...`
  tail has no parameter to disagree with, and `printf("%p", fn)` must keep working.
- Adds a per-candidate explanation line to the "no overload matches" dump, since opaque pointers
  make it print two indistinguishable `ptr`s.
- 7 reject legs and value legs, each mutation-tested in isolation (exit 1 on a `904f026` binary,
  0 on the branch).

**The two findings that stopped it:**

1. **MEDIUM - the diagnostic loop misses the variadic gate's own cell.**
   `cflat/LLVMBackend.h:18437` applies the scorer's empty-`TypeName` shape requirement to every
   candidate, but the variadic gate does not require that shape - it calls `ArgumentIsCodeValue`
   unconditionally. A `Lambda<T>` FAT value into `int lam(Rec* r, ...)` is `r=902` on master and
   correctly rejected on the branch, but gets no explanation line at all. The condition has to be
   per-arm (empty `TypeName` for the two scorer sites, unconditional for the variadic one), or the
   loop needs to know which arm refused. There is also no test leg for this cell: all seven reject
   legs and all three new value legs use a thin `function<>` value, so fat-value-into-variadic is
   unprobed on BOTH the reject and the accept side.

2. **LOW - a code value into a variadic's declared SCALAR parameter is still unjudged.**
   `ParameterStoresData` returns false for a non-pointer scalar, so `int lam(int n, ...)` competing
   with a funcptr overload produces a raw LLVM verifier failure on both binaries:
   `Call parameter type does not match function signature!`. CLAUDE.md's rule is to turn a
   diagnosed LLVM-level failure into a proper compiler error. Variadic-specific - the non-variadic
   twin `int lam(int n)` rejects cleanly with the normal "no overload matches" on both binaries.
   Pre-existing, but the branch's landed record claims the variadic hole is closed, and this is the
   one variadic shape it does not cover.

**Settled along the way, do not re-litigate:**

- The `ParameterStoresData` vs raw `Pointer` asymmetry at ~17862 is CORRECT, proven not assumed:
  the only two cases where they differ are `IsFunctionPointer` and `IsEncodedClosureType`, which is
  exactly the funcptr arm's first conjunct, and `ArgumentIsCodeValue` implies its second - so the
  arm claims those parameters and the else branch is unreachable for them.
- Dropping the variadic candidate cannot change which other candidate wins in a call with no code
  value at all. Structural, not empirical: the drop is conditioned on a code value at a declared
  data parameter.
- Core variadics are safe by construction - `printf`/`sprintf`/`snprintf`/`fprintf`/`scanf`/
  `sscanf`/`fscanf` and `os.posix.open` declare only `char*`/`void*`/scalar prefixes, and
  `cflat/core/cocoa.cb` already passes IMPs through explicit `(void*)imp` casts.
- The branch also files two issues that should land with it:
  `p2/c-binder-misses-decorated-function-pointer-parameter` (a `const`-qualified C function-pointer
  parameter defeats the literal `"(*)"` probe and binds as `void*`; the `atexit` rejection is
  separately caused by a hand-written `extern int atexit(void* func);` at `cflat/core/cruntime.cb:584`)
  and `p1/code-value-into-data-pointer-outside-overload-resolution` (`Rec* r = w;`, `return w;`, and
  a field store are exit 138 with no diagnostic, identical on both binaries - the store paths this
  fix deliberately does not reach).
