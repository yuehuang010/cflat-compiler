# G3: SIMD Completeness - Detailed Plan

Status: PROPOSED (not started)
Created: 2026-07-09
Parent: internal/plan/hpc-gaps.md (G3)

Goal: add horizontal reductions, masked load/store, and gather/scatter to
`simd<T,N>`. Each op is a 1:1 lowering to a target-independent LLVM
intrinsic (per internal/simd-type.md: portable `llvm.vector.reduce.*` /
`llvm.masked.*`, never `llvm.x86.*`). No grammar changes, no type-syntax
changes - everything hooks into the existing static-method dispatch.

Explicitly out of scope (deferred to their own plan when a kernel needs
them): shuffles/permutes, lane writes (`v[i] = x` stays an error),
`load_aligned` variants (those belong to G1's follow-on).

---

## Where everything hooks in (from codebase survey 2026-07-09)

- Central dispatch: `ParseSimdStaticMethod` in MainListener.h (~11038),
  string-matches the method name (`load` ~11067, `store` ~11079,
  `LookupSimdMathIntrinsic` ~11092, `clamp`, `sign`, `select` ~11147,
  trailing "unknown method" error ~11165). New ops = new branches here.
- Intrinsic mechanism: `llvm::Intrinsic::getDeclaration(module, Id,
  {overloadTys})` + `builder->CreateCall` (see the vector-math path
  ~11103). Reuse verbatim.
- Element alignment for memory ops: `getABITypeAlign(elemTy)` (~11058);
  GEP element-index bridge (~11072). Masked load/store reuse both.
- Mask representation: `<N x i1>` (= `simd<bool,N>`). Validation
  pattern to copy: `select`'s mask check (~11154-11157).
- Arg-shape validation: `requireSimdArg` (~11174). Errors:
  `LogErrorContext(ctx, msg)` - log-and-continue style, ASCII text.
- Neither `ParseDeclarationSpecifiers` copy is touched (no new type
  syntax). Verify this stays true at review time (G3 constraint from
  the parent plan).
- No existing `llvm.vector.reduce.*` / `llvm.masked.*` use anywhere in
  cflat - all three phases are greenfield.

---

## Phase 1: Horizontal reductions (highest value)

API (static methods, consistent with existing `simd<T,N>.load(...)`):

```cflat
T simd<T,N>.reduce_add(simd<T,N> v);
T simd<T,N>.reduce_min(simd<T,N> v);
T simd<T,N>.reduce_max(simd<T,N> v);
```

Result is scalar T (drop IsSimd on the returned TypeAndValue, like the
lane-read path ~12498 does).

Lowering:

| Method | Float lanes | Int lanes |
|--------|-------------|-----------|
| reduce_add | `llvm.vector.reduce.fadd(start, v)` with `reassoc` FMF on the call, start = +0.0 | `llvm.vector.reduce.add(v)` |
| reduce_min | `llvm.vector.reduce.fmin(v)` | `smin` / `umin` by signedness |
| reduce_max | `llvm.vector.reduce.fmax(v)` | `smax` / `umax` by signedness |

Decisions (settled here so implementation does not re-litigate):
- `reduce_add` on float is REASSOCIATING (`reassoc` flag on the fadd
  reduce call). This matches the vectorize contract's documented
  reduction semantics (a `vectorize` reduction loop already reorders).
  Document it in HPC.md next to the op. No ordered variant in this
  pass; `reduce_add_ordered` can be added later if anyone asks.
- The fadd/fmul reduce intrinsics take an explicit start operand -
  pass scalar +0.0 of the element type.
- fmin/fmax reduce follow llvm.minnum/maxnum NaN semantics - same
  family the existing `min`/`max` vector math already uses (~11187
  table maps min->minnum), so semantics stay consistent. Note in doc.
- Signedness for int min/max comes from the element TypeAndValue the
  same way CreateVectorOperation picks signed vs unsigned compares
  (LLVMBackend.h ~10399-10414) - follow that code's convention.
- bool (`i1`) lanes: reject reductions with a clear error (use
  select/mask ops instead); keeps the matrix small.

Errors: wrong arity ("reduce_add expects 1 argument"), non-simd arg
(reuse requireSimdArg), bool-lane reject. Extend the trailing
valid-method-list error (~11167) with the new names.

## Phase 2: Masked load/store (kills the n % N scalar tail)

API:

```cflat
simd<T,N> simd<T,N>.load_masked(T[] arr, long i, simd<bool,N> mask,
                                simd<T,N> passthru);
void      simd<T,N>.store_masked(simd<T,N> v, T[] arr, long i,
                                 simd<bool,N> mask);
```

Arg order mirrors existing `load(arr, i)` / `store(v, arr, i)` with the
mask (and passthru) appended - kernels migrate by appending arguments.
`passthru` is required, not defaulted: an explicit zero splat is one
argument and keeps dispatch dumb (no optional-arg machinery).

Lowering: `llvm.masked.load(ptr, align, mask, passthru)` /
`llvm.masked.store(v, ptr, align, mask)`. Pointer = same GEP
element-index bridge as load/store (~11072); align = same
`getABITypeAlign(elemTy)` (~11058). Overload types: masked.load is
overloaded on {vecTy, ptrTy}; masked.store on {vecTy, ptrTy} - check
the exact overload signature against the vcpkg LLVM headers rather
than guessing.

Mask validation: copy select's check (FixedVectorType of i1, matching
lane count, ~11154-11157). Whatever arr/index validation `load`/`store`
already do applies unchanged.

Canonical use (document this loop in HPC.md - it is the whole point):

```cflat
// tail-free loop: one masked iteration replaces the scalar epilogue
for (long i = 0; i < n; i += N) { ... lane-index compare builds mask ... }
```

Building the tail mask needs a lane-index vector compared against a
splat of the remaining count. If there is no ergonomic way to produce
{0,1,2,...,N-1} today, add `simd<T,N>.lanes()` (constant iota via
ConstantVector) in this phase - one more trivial branch in the same
dispatch, and without it masked ops are unusable in practice.

## Phase 3: Gather/scatter (enables CSR spmv, particle codes)

API:

```cflat
simd<T,N> simd<T,N>.load_gather(T[] arr, simd<int,N> idx);
void      simd<T,N>.store_scatter(simd<T,N> v, T[] arr, simd<int,N> idx);
```

- idx lanes: accept `simd<int,N>` and `simd<long,N>` (i32/i64). Reject
  float/bool index vectors with a clear error.
- MVP: unmasked (mask operand = all-true splat constant). Masked
  overloads can be added later by arity if a kernel needs them;
  do not build optional-mask plumbing now.
- Lowering: build `<N x ptr>` with a single `CreateGEP(elemTy, basePtr,
  idxVec)` (vector index on a scalar base yields a vector of pointers),
  then `llvm.masked.gather(ptrVec, align, allTrue, passthru=undef... )`
  - NOTE: pass `poison`/`undef` passthru for the unmasked gather;
  `llvm.masked.scatter(v, ptrVec, align, allTrue)`. Align =
  `getABITypeAlign(elemTy)`.
- Semantics note for the doc: scatter with duplicate indices stores in
  lane order (last lane wins) - that is LLVM's defined behavior; state
  it so users are not surprised.

## LSP

Check whether simd static methods are surfaced anywhere in completion
(LspServer/LspSymbolIndex). If there is a hardcoded method list, extend
it; if completion is generic, nothing to do. Run test_lsp.bat either way
since MainListener.h is touched.

---

## Verification (per phase, cumulative)

1. `.ll` inspection (`--out-lli`): each op emits exactly its intrinsic -
   `llvm.vector.reduce.fadd` with `reassoc`, `llvm.masked.load/store`
   with correct align + mask, `llvm.masked.gather/scatter` on
   `<N x ptr>`. Spot-check at -O0 and confirm -O2 does not
   miscompile/split them.
2. Functional tests: extend the simd sections of `Test/test_hpc.cb`
   (NOT a new test file):
   - reduce_add/min/max: float + double + int + unsigned lanes; compare
     against a scalar loop over v[i]. For reduce_add float, pick
     exactly-representable values (floats default trap - see memory).
   - masked: partial-tail load/store roundtrip vs scalar reference,
     passthru lanes verified untouched, store_masked leaves
     masked-off memory unmodified.
   - gather/scatter: permutation indices roundtrip; scatter
     duplicate-index lane-order check.
3. Error tests: new `Test/errors/err_simd_*.cb` files (sanctioned path -
   error tests are the one place new files are expected): bad arity,
   bad mask type, bool-lane reduce, float index vector on gather.
4. Perf evidence: dot-product / spmv style kernel in performance/hpc
   with the scalar tail replaced - `performance.bat` with `-O2`, expect
   the tail cost to disappear for n % N != 0 sizes.
5. Gates: build Release, `test.bat` green, `test_lsp.bat` green
   (MainListener touched), `example.bat` green. Update `doc/HPC.md`
   (simd section ~85+: new ops in the intrinsic table, reassoc
   semantics note, tail-free loop example, scatter ordering note) in
   the same change.

## Sequencing / delegation

Three phases land independently (each is green-gated on its own).
Phase 1 is small and mechanical once decisions above are fixed - sonnet
tier. Phases 2-3 involve mask/pointer-vector subtleties and doc work -
sonnet with this plan as the prompt, escalate to opus on flail.
Conflicts with G1 (alignas-on-new) are confined to ParseNewExpression /
grammar - disjoint from ParseSimdStaticMethod, safe to run in parallel.
