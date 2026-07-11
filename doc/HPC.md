# HPC & SIMD Features

CFlat's high-performance-computing features share a single philosophy: the compiler
*proves* what other languages leave to hope and `#pragma`. Auto-vectorization is a
**checked contract** rather than a silent best-effort, and explicit SIMD is a
first-class primitive type rather than a library of intrinsics.

The two features are complementary halves of the within-core (SIMD) parallelism axis:

- **`vectorize`** - keep writing a scalar loop, and have the compiler *prove* it
  vectorized (or hard-error). The portable default.
- **`simd<T,N>`** - write the vector values yourself for a *guaranteed* result. The
  escape hatch for when auto-vectorization can't or won't fire.

A third feature, the **`T[]` array-view**, removes the *reason* a `vectorize` loop most
often needs a runtime guard: pointer aliasing. It is the canonical fix a vectorization
failure points you toward.

## Table of Contents

- [`vectorize` loops](#vectorize-loops)
- [`simd<T,N>` explicit vector type](#simdtn-explicit-vector-type)
- [`T[]` array-view (noalias)](#t-array-view-noalias)
- [Alignment control (`alignas`)](#alignment-control-alignas)
- [`span<T>` (noalias) and `view<T>` (may-alias)](#spant-noalias-and-viewt-may-alias)
- [Across-core data parallelism (`parallel_for_n` / `parallel_reduce`)](#across-core-data-parallelism-parallel_for_n--parallel_reduce)
  - [NUMA domains (`core/numa.cb`)](#numa-domains-corenumacb)
  - [Dynamic scheduling (`parallel_for_n_dynamic` / `parallel_for_n_dynamic_pool`)](#dynamic-scheduling-parallel_for_n_dynamic--parallel_for_n_dynamic_pool)
    - [`parallel_reduce_dynamic<T>` / `parallel_reduce_dynamic_pool<T>` - dynamic reduction](#parallel_reduce_dynamict--parallel_reduce_dynamic_poolt---dynamic-reduction)
- [Huge pages](#huge-pages)
- [Memory-mapped files (`MappedFile`)](#memory-mapped-files-mappedfile)
- [Numeric kernel libraries (`core/hpc`)](#numeric-kernel-libraries-corehpc)

**Related:** [Threading & Memory Management](THREADING.md) (threads, affinity, thread pool, allocators, lock-set analysis) | [Language & Core Library Features](LANGUAGE.md)

---

## `vectorize` loops

`vectorize` is a prefix modifier on a counted `for` or `while` loop that turns
auto-vectorization from a silent best-effort into a **checked contract**: the loop
must be vectorized by the optimizer, or the compile fails with an error.

```c
vectorize for (int k = 0; k < n; k = k + 1)
{
    c[k] = a[k] + b[k];      // elementwise add - vectorizes
}

vectorize while (k < n)      // while form: condition must be countable
{
    sum = sum + a[k];        // integer reduction - vectorizes
    k = k + 1;
}
```

Semantics:

- **Enforced only at `-O2`.** The loop vectorizer runs only at `-O2`, so that is the
  only level where `vectorize` is enforced. At `-O0`/`-O1` it is a no-op and the loop
  compiles as an ordinary loop. (Build with `-O2` to get the guarantee.)
- **Hard error on failure.** If the optimizer cannot vectorize a marked loop, the
  compile fails, e.g. `vectorize loop could not be vectorized: ...`. This catches
  the silent regression where a loop quietly stops vectorizing after an edit.
- **A surviving runtime alias check is also a failure.** `vectorize` promises the
  *clean*, unconditional vector loop. When the compiler cannot prove the loop's
  pointers disjoint, the optimizer would guard the vector path with a runtime overlap
  test (loop versioning) plus a scalar fallback - that is rejected with
  `vectorize loop did not vectorize cleanly: a runtime alias check remained ...`. The
  fix is to give the optimizer the disjointness it needs: pass the buffers as
  `span<T>` / `T[]` (noalias). For a span, index it directly (`y[i]`), use `y.get(i)` /
  `y.set(i, v)`, or bind a local `T[] v = y.data();` - all three carry the receiver's
  noalias scope. (The accessor forms only carry it for an addressable span; an rvalue
  span like `makeSpan().get(i)` falls back to the method call and keeps the check.)
- A loop also fails the contract when it is not vectorizable at all: no countable trip
  count (e.g. `vectorize while (p != nullptr)` over a linked list), a loop-carried
  dependence (`a[k] = a[k-1] + 1`), or a non-inlinable call in the body.
- This list is not exhaustive: for unusual access patterns (strided or interleaved,
  e.g. re/im pairs in `buf[2*k]` / `buf[2*k+1]`), vectorizability may vary by pattern
  and target. The cheap way to find out is to mark the loop `vectorize` and compile:
  the contract either holds or fails loudly - there is no silent-scalar outcome.
- Integer reductions (`sum += a[k]`) vectorize. Floating-point reductions also
  vectorize: the optimizer reassociates the additions (and reduces the lanes at the
  end), which reorders the sums and so can change the result in the last few ULPs.
  Mark a loop `vectorize` only when that reassociation is acceptable. For a parallel
  FP reduction where you want to control the accumulation explicitly, use
  `parallel_reduce<T>` instead (see below). To additionally fuse the `a[k] * b[k]`
  products in a dot-product reduction into single fused-multiply-adds, use
  `vectorize(contract)` - see the FP-math tiers below.
- Only counted `for` and `while` are allowed; `do`/`while` and `foreach` are rejected
  up front (`foreach` lowers to `count()`/`get()` calls that cannot vectorize).

### FP-math tiers: `vectorize(contract)` and `vectorize(reassoc)`

Plain `vectorize` emits every floating-point operation in the loop body with default
(strict) rounding: a `mul` then an `add` are two separately-rounded steps, and the
optimizer may not regroup them. An optional parenthesized flag relaxes FP math for the
loop's lexical body only. The flags are **tiers**, not composable options - `reassoc`
implies `contract`:

```c
vectorize(contract) for (int k = 0; k < n; k = k + 1)
{
    out[k] = a[k] * b[k] + c[k];   // mul+add may fuse to one fma on FMA targets
}

vectorize(reassoc) for (int k = 0; k < n; k = k + 1)
{
    dot = dot + a[k] * b[k];       // products fuse AND the sum may be regrouped
}
```

- **`vectorize(contract)`** sets the `contract` fast-math flag on the body's FP ops.
  This lets the backend fuse a multiply feeding an add (`a*b + c`) into a single
  fused-multiply-add (`vfmadd...` on AVX2/FMA targets), removing one intermediate
  rounding step. It never reorders across statements - the "safe" tier. On a dot-
  product reduction it stacks with the existing reduction reassociation, so the vector
  body both fuses each product and accumulates in vector lanes (the dot-product payoff).
- **`vectorize(reassoc)`** sets `reassoc` in addition to `contract`. It extends the
  reordering permission that plain `vectorize` already grants to the reduction chain to
  ALL FP math in the body: the optimizer may regroup expressions and split the
  reduction into several independent accumulators. Results can differ from a strict
  serial computation by more than the reduction ULP drift, so this is the tier for
  "throughput over reproducibility" kernels.
- The flags apply to the **lexical body** only - the loop condition/increment (integer
  ops in a counted loop) and any code outside the loop stay strict, as do sibling and
  outer plain loops. A lambda written inside a `vectorize(contract)`/`vectorize(reassoc)`
  body is emitted inline through the same builder, so its FP ops inherit the tier too -
  defensible lexical-scope semantics, and it does not leak past the loop.
- Any flag other than `contract` or `reassoc` (e.g. `vectorize(fast)`) is a compile
  error: `vectorize flag must be 'contract' or 'reassoc'`.
- The flags are emitted at all optimization levels (harmless at `-O0`, where ISel may
  still fuse a contracted mul-add); the vectorization contract itself is still enforced
  only at `-O2`, unchanged. `contract` neither helps nor hurts vectorizability.
- **`contract` is permission to fuse, not a guarantee.** For an exact, single-rounding
  fused-multiply-add regardless of target or opt level, call `fma()` (see
  `core/math.cb`) or use `simd`'s `fma` - those always lower to `llvm.fma`.

## `simd<T,N>` explicit vector type

`simd<T,N>` is a fixed-width SIMD value: `N` lanes of a numeric scalar `T`, held in a
vector register and operated on all at once. It lowers directly to the LLVM vector type
`<N x T>`, so element-wise arithmetic on it is a single hardware instruction rather than a
loop. Where `vectorize` is *best-effort* auto-vectorization of a scalar loop, `simd<T,N>`
is the *guaranteed* counterpart: the vector operations exist by construction.

```c
simd<float, 8> a = 2.0;        // splat: every lane = 2.0
simd<float, 8> b = 3.0;
simd<float, 8> c = a * b;      // one vector multiply -> 6.0 per lane
simd<float, 8> d = c + a;      // 8.0 per lane
float first = d[0];            // read lane 0 -> 8.0

simd<i32, 4> p = 5;
simd<i32, 4> q = p * p;        // 25 per lane
```

- **Element type** `T` must be a numeric scalar (`i8`-`i64`, `u8`-`u64`, `float`, `double`).
  `bool` and struct/pointer types are rejected.
- **Lane count** `N` must be a **power-of-two integer literal** in `[2, 64]`. A non-power-of-two
  (e.g. `simd<float,7>`) is a compile error - the explicit SIMD type exists to control the
  hardware mapping, so a count that would silently waste a lane is rejected with a hint
  (`did you mean simd<...,8>?`).
- **Construction**: a scalar initializer is *splatted* across all lanes (`simd<float,8> v = 1.0;`).
- **Operators**: element-wise `+ - * /`. Either operand may be a scalar, which is splatted to
  match (`a * 2.0`, `10.0 - a`); both vector operands must share lane count and element type.
- **Lane access**: `v[i]` reads lane `i`. Lanes are *read-only* - there is no lane write
  (`v[i] = x`); build a new vector or use `simd<T,N>.load`/`.store` (below) to move data.
- **Memory bridge**: `simd<T,N>.load` / `simd<T,N>.store` move a whole vector between a
  `simd<T,N>` and a `T[]`/`T*` buffer (see below). This is the supported way to get real data
  into and out of a vector, since individual lanes are not addressable.
- **Vector math**: `simd<T,N>.sqrt`, `.abs`, `.min`, `.max`, `.clamp`, `.sign`, `.fma`, ... are
  static methods that apply an elementwise math op to whole vectors, each lowering to a true
  SIMD instruction (see below). FP element types only.
- **Comparisons** (`== != < <= > >=`) yield a `simd<bool,N>` mask for use with `.select` -
  see [Comparisons and `select`](#comparisons-and-select---branchless-masking) below.
- It is a **primitive value**, not a struct or array - no ownership, destructor, or `move`
  interaction. (Lane writes and shuffles are not yet supported.)

### `simd<T,N>.load` / `simd<T,N>.store` - the memory bridge

A `simd<T,N>` lives in a register; to fill it from (or spill it to) an array you load/store the
whole vector at once. `load` and `store` are static methods on the type itself - the `<T,N>` on
the call makes them self-describing, so they work with `auto` and compose inside larger
expressions:

```c
double[] a = new double[1024];

auto v = simd<double,4>.load(a, i);    // v = { a[i], a[i+1], a[i+2], a[i+3] }
v = v * v;                             // whole-vector arithmetic
simd<double,4>.store(v, a, i);         // a[i .. i+4] = v's lanes
```

- `simd<T,N>.load(array, index)` returns a `simd<T,N>` filled from `N` contiguous elements
  starting at `array[index]`.
- `simd<T,N>.store(vec, array, index)` writes `vec`'s `N` lanes to `array[index .. index+N]`.
  `vec` must be a `simd<T,N>` of the same shape as the receiver type.
- `index` is an **element** offset, not bytes. `array` is any `T[]` or `T*`.
- Natural **element** alignment is used (the load/store is `align alignof(T)`, not `align N*sizeof(T)`),
  so an arbitrary index into an ordinary buffer is safe - the buffer need not be vector-aligned.
- Because the type args are explicit, the calls nest directly - a fused load/op/store needs no
  named temporary:

  ```c
  // square a[i .. i+4] in one expression
  simd<double,4>.store(simd<double,4>.load(a, i) * simd<double,4>.load(a, i), a, i);
  ```

- A strided kernel walks the buffer in chunks of `N`:

  ```c
  int i = 0;
  while (i + 4 <= n) {
      auto c = simd<double,4>.load(a, i);
      simd<double,4>.store(c * c, a, i);
      i = i + 4;
  }
  // ... handle the tail (n % 4) with scalar code ...
  ```

### Vector math - `simd<T,N>.sqrt`, `.min`, `.fma`, ...

Beyond `+ - * /`, the common floating-point math ops are available as static methods on the
type. Each maps **1:1 to an LLVM vector intrinsic** that lowers to a real SIMD instruction
(`sqrtpd`, `roundpd`, `vfmadd...`, `minpd`, ...) - no scalarization, no `libm` call. Like
`load`/`store`, the explicit `<T,N>` makes them self-describing, so they compose and work with
`auto`:

```c
simd<double,4> r2  = simd<double,4>.load(rsq, i);
simd<double,4> inv = one / simd<double,4>.sqrt(r2);          // reciprocal sqrt, vectorized
simd<double,4> acc = simd<double,4>.fma(inv, dx, acc);       // acc = inv*dx + acc, one vfmadd
```

| Method | Arity | Lowers to | Notes |
|--------|-------|-----------|-------|
| `sqrt(v)` | 1 | `llvm.sqrt` (`sqrtps`/`sqrtpd`) | |
| `abs(v)` / `fabs(v)` | 1 | `llvm.fabs` | bitwise, no branch |
| `floor` / `ceil` / `trunc` / `round` / `rint` `(v)` | 1 | `llvm.floor` etc. (`roundps`) | SSE4.1 round |
| `min(a, b)` / `max(a, b)` | 2 | `llvm.minnum` / `maxnum` | NaN-ignoring IEEE min/max (`minpd`) |
| `copysign(mag, sign)` | 2 | `llvm.copysign` | magnitude of `mag`, sign of `sign` |
| `clamp(x, lo, hi)` | 3 | `maxnum` then `minnum` | `min(max(x,lo),hi)`, elementwise |
| `sign(x)` | 1 | `copysign` + compare/select | `-1`/`0`/`+1` per lane (`+-0` -> `0`; a NaN lane -> `+-1`) |
| `fma(a, b, c)` | 3 | `llvm.fma` (`vfmadd`) | `a*b + c`, single rounding - **see the FMA caveat below** |

- **FP element types only.** These are floating-point intrinsics; calling one on an integer
  `simd` (e.g. `simd<i32,4>.sqrt`) is a compile error.
- **`fma` needs FMA *hardware* to be fast.** `fma` is a *single-rounded* multiply-add, so it
  is **not** interchangeable with `a*b + c` (which rounds twice). On a target *without* an FMA
  unit - and cflat's default build targets generic `x86-64`, which has none - LLVM cannot
  lower `llvm.fma` to a vector `fmulpd`/`faddpd` pair without changing the result, so it falls
  back to a **per-lane libm `fma()` call**. That is correct but slow: in the N-body kernel it
  *halved* throughput (46 -> 19 GFLOP/s) versus plain `acc + dx*s`. Reach for `simd.fma` only
  when you specifically need the extra precision of single rounding *and* know the target has
  FMA; for a plain accumulate, write `acc + a*b` and let it vectorize to `fmul`+`fadd`. To let
  the backend *fuse* that `fmul`+`fadd` into a hardware `vfmadd` when the target has FMA (and
  fall back to the unfused pair otherwise), wrap the loop in `vectorize(contract)` - see
  "FP-math tiers" under the `vectorize` section above.
- **Every operand is a `simd<T,N>` of the same shape** - there is no implicit scalar splat
  here (unlike `+ - * /`). Splat first if you need a constant: `simd<double,4> k = 0.5;`.
- **Transcendentals are deliberately excluded.** `exp`, `log`, `sin`, `cos`, `pow`, ... have
  no hardware vector form; without a vector-math runtime (SVML/libmvec, which we do not link)
  they would scalarize to per-lane `libm` calls - a vector op in name only. They are left out
  rather than ship a "SIMD" call that secretly runs one lane at a time. For those, keep the
  loop on `vectorize` (the auto-vectorizer makes the same call explicitly) or write a scalar
  loop.

### Comparisons and `select` - branchless masking

A numerical kernel is rarely all arithmetic: a stencil reflects at a wall, a clamp picks a
branch, a solver masks out dead cells. Written as a scalar `if`, that data-dependent branch
is exactly what stops a loop from vectorizing. `simd<T,N>` closes the gap with a **vector
comparison** and a **branchless `select`**, so the masked body stays straight-line vector code.

A comparison operator on two `simd<T,N>` values (or a vector and a splatted scalar) yields a
**`simd<bool,N>` mask** - one `i1` lane per element, lowering to a single `vcmppd`/`pcmpgtd`:

```c
simd<double,4> u   = simd<double,4>.load(speed, i);
simd<double,4> lim = 1.0;
simd<bool,4>   hot = u > lim;          // vector compare -> mask (a simd<bool,N>)
```

- All six operators are supported: `== != < <= > >=`. Floating-point uses **ordered**
  predicates (a `NaN` lane compares false); integer relations honour the operands' signedness
  (`u32` lanes use the unsigned predicate).
- The mask type is `simd<bool,N>` - the *one* place a `bool` element is allowed (it is not an
  arithmetic scalar, so you cannot `+ - * /` a mask). Store it in an explicit `simd<bool,N>`
  local, **or let `auto` deduce it**, or feed the comparison straight into `select`:

  ```c
  simd<bool,4> hot = u > lim;     // explicit
  auto         hot = u > lim;     // auto - deduces simd<bool,4>, identical value/behaviour
  ```

`simd<T,N>.select(mask, a, b)` is the per-lane blend `mask ? a : b`, lowering to a single
vector select (`vblendvpd`):

```c
// LBM bounce-back: solid cells reflect to the wall value, fluid cells take the relaxed
// value. The branchy `solid ? wall : value` becomes one compare + one select - no branch,
// so the chunked loop is pure vector code and vectorizes at -O2.
simd<bool,4>   solid  = simd<double,4>.load(mask, i) > half;
simd<double,4> out    = simd<double,4>.select(solid, wall, relaxed);
simd<double,4>.store(out, fout, i);
```

- `mask` must be a `simd<bool,N>` of the **same lane count** as `a`/`b` (typically the result
  of a comparison); `a` and `b` are `simd<T,N>` of this receiver's shape.
- Works for **any** element type, not just floating point - blending values is type-agnostic,
  so an integer `select` is fine.
- Like `load`/`store`/the math ops, the explicit `<T,N>` makes the call self-describing, so it
  composes inside larger expressions and works with `auto`.

This is the missing primitive for porting a *branchy* kernel (bounce-back, masked relaxation,
piecewise functions) to SIMD: hoist the condition into a mask, blend with `select`, and the
inner loop has no data-dependent branch left to block vectorization. A worked end-to-end
example (a bounce-back/relax step, with a scalar reference for bit-equality) is in
[`eval/cfd/simd_bounceback_proof.cb`](../eval/cfd/simd_bounceback_proof.cb).

### Horizontal reductions - `simd<T,N>.reduce_add` / `.reduce_min` / `.reduce_max`

Collapse all `N` lanes of a `simd<T,N>` to a single scalar `T`:

```c
simd<double,4> v     = simd<double,4>.load(a, i);
double         total = simd<double,4>.reduce_add(v);   // sum of the 4 lanes
double         lo    = simd<double,4>.reduce_min(v);
double         hi    = simd<double,4>.reduce_max(v);
```

| Method | Lowering (float) | Lowering (int) |
|--------|-------------------|-----------------|
| `reduce_add` | `llvm.vector.reduce.fadd` (start = +0.0, `reassoc` flag) | `llvm.vector.reduce.add` |
| `reduce_min` | `llvm.vector.reduce.fmin` | `llvm.vector.reduce.smin` / `.umin` by signedness |
| `reduce_max` | `llvm.vector.reduce.fmax` | `llvm.vector.reduce.smax` / `.umax` by signedness |

- **`reduce_add` on float/double is reassociating** - the call carries the `reassoc` fast-math
  flag, so the compiler is free to sum the lanes in any order (a tree, pairwise, whatever the
  target's reduction instruction does). This matches the `vectorize` reduction contract, which
  already reorders a scalar accumulation loop. There is no ordered variant in this pass.
- `reduce_min`/`reduce_max` on floating point follow the same `minnum`/`maxnum` NaN family as
  `simd<T,N>.min`/`.max` (a NaN operand does not "poison" the result if the other operand is a
  number) - semantics stay consistent across the vector-math and reduction APIs.
- Integer min/max pick the signed or unsigned family from `T`'s signedness (`u8..u64` -> unsigned,
  everything else -> signed), the same convention `simd<T,N>`'s comparison operators use.
- `bool` (`simd<bool,N>`) lanes are rejected with a clear error - reduce a mask with `select`
  or ordinary boolean logic instead; this keeps the reduction matrix small.

### Building a lane-index vector - `simd<T,N>.lanes()`

`simd<T,N>.lanes()` returns the constant `{0, 1, 2, ..., N-1}` as a `simd<T,N>` (an iota). It
exists to build a **tail mask**: compare `lanes()` against a splat of the remaining element
count to get exactly the mask a partial last iteration needs.

### Masked load/store - the tail-free loop

`simd<T,N>.load_masked` / `simd<T,N>.store_masked` are `load`/`store` with a per-lane mask
appended, so a masked chunk can stand in for the scalar epilogue that a fixed-width SIMD loop
usually needs when the element count is not a multiple of `N`:

```c
simd<T,N> simd<T,N>.load_masked(T[] arr, long i, simd<bool,N> mask, simd<T,N> passthru);
void      simd<T,N>.store_masked(simd<T,N> v, T[] arr, long i, simd<bool,N> mask);
```

```c
// Tail-free loop: one masked final iteration replaces the scalar epilogue that would
// otherwise handle `n % N != 0`. remaining <= 0 in the mask compare means "past the end".
i64 i = 0;
while (i + N <= n)
{
    simd<double,N> v = simd<double,N>.load(a, i);
    simd<double,N>.store(v * v, a, i);
    i = i + N;
}
if (i < n)
{
    simd<double,N> remaining = (double)(n - i);
    simd<bool,N>   mask      = simd<double,N>.lanes() < remaining;
    simd<double,N> passthru  = 0.0;
    simd<double,N> v = simd<double,N>.load_masked(a, i, mask, passthru);
    simd<double,N>.store_masked(v * v, a, i, mask);
}
```

- `passthru` is a **required** argument, not defaulted: lanes where `mask[k]` is false take
  `passthru[k]` instead of reading memory. An explicit zero (or sentinel) splat is one argument
  and keeps dispatch simple - there is no optional-argument machinery.
- `store_masked` leaves the destination memory for masked-off lanes **completely untouched**
  (it is not a read-modify-write; those bytes are simply not written).
- Both lower 1:1 to `llvm.masked.load` / `llvm.masked.store` at the same element alignment
  `load`/`store` already use.

### Gather/scatter - `simd<T,N>.load_gather` / `simd<T,N>.store_scatter`

```c
simd<T,N> simd<T,N>.load_gather(T[] arr, simd<int,N> idx);
void      simd<T,N>.store_scatter(simd<T,N> v, T[] arr, simd<int,N> idx);
```

Reads (or writes) `N` elements of `arr` at `N` independent, dynamically-computed indices in one
vector instruction - the primitive that CSR sparse matrix-vector products and particle codes
need (`row[j]`, `col[j]` are not contiguous):

```c
simd<i32,4>    idx  = simd<i32,4>.load(colIndices, k);      // 4 sparse column indices
simd<double,4> vals = simd<double,4>.load_gather(x, idx);   // x[idx[0..3]]
simd<double,4>.store_scatter(vals * scale, y, idx);         // y[idx[0..3]] = vals*scale
```

- `idx` must be `simd<int,N>` or `simd<long,N>` (i32 or i64 lanes); a float or bool index vector
  is a compile error.
- **MVP is unmasked**: every lane is always read/written (the mask operand passed to the
  underlying `llvm.masked.gather`/`llvm.masked.scatter` is an all-true constant). A masked
  overload can be added later, by arity, if a kernel needs partial gather/scatter.
- `load_gather`'s passthru is `poison` - there is no masked-off lane to preserve, since every
  lane is always read.
- **Scatter with duplicate indices stores in lane order - the last lane targeting a given
  address wins.** This is `llvm.masked.scatter`'s defined behaviour; if two lanes of `idx` are
  equal, whichever has the higher lane number determines the final value at that address.
- Lowering: a single `CreateGEP` with a *vector* index and a scalar base pointer yields the
  `<N x ptr>` that `llvm.masked.gather`/`llvm.masked.scatter` take directly - no manual
  per-lane pointer arithmetic.

## `T[]` array-view (noalias)

`T[]` (empty brackets) is a distinct type from `T*`: a **thin array-view**. It has the
exact same representation as a pointer - just `T*` under the hood, no length is carried -
but it adds a **noalias contract**: distinct `T[]` values point at distinct, *whole*
allocations.

```c
// Two views are provably disjoint -> each is noalias -> the loop vectorizes at -O2
// with NO runtime alias check (no loop versioning, no scalar fallback).
void axpy(int[] y, int[] x, int n, int k)
{
    int i = 0;
    while (i < n) { y[i] = y[i] + k * x[i]; i = i + 1; }
}

int[] a = new int[n];   // `new T[n]` yields a view: a fresh, whole, distinct allocation
int[] b = new int[n];
axpy(a, b, n, 2);
```

Contrast with C, where `int[]` as a parameter silently decays to `int*` (same type, no
guarantee). CFlat makes the distinction real: `int[]` is the *stronger* type you opt into
by spelling, and `int*` stays the raw, may-alias, arithmetic-capable C pointer. Migrate a
hot kernel to `int[]` deliberately; existing `int*` code is never reinterpreted.

Rules:

- **noalias by spelling.** Writing `int[]` emits LLVM `noalias` on that parameter, so the
  loop vectorizer drops the runtime overlap check it would otherwise insert for `int*`
  (see the *runtime alias checks* note under `vectorize`). This is the headline win.
- **No pointer arithmetic.** `a + i`, `a++`, `a--` are compile errors on a view - index it
  with `a[i]` instead. This is what makes the noalias contract *provable* rather than
  trusted: with no arithmetic (and no `int* -> int[]` conversion), a view can only ever
  span a whole allocation, so two views alias if and only if they share a base pointer.
- **One-way conversion, enforced - with an explicit escape.** Decay `T[] -> T*` is implicit
  and always safe (it drops the guarantee). The reverse - *implicitly* binding a raw `T*` into
  a `T[]` - is a **compile error** at every binding site: declaration-init (`int[] a = p;`),
  re-assignment (`a = p;`), struct field store (`s.view = p;`), passing to a `T[]` parameter
  (`f(p)`), and `return p;` from a `T[]`-returning function. This is the same shape as the
  language's implicit-upcast / explicit-downcast rule: the unsafe direction (adding an
  *unprovable* guarantee) needs an **explicit cast** `(T[])p`, by which you assert the pointer
  spans a whole, distinct allocation:

  ```c
  int* p = some_c_api();        // a raw, whole buffer you own
  int[] a = (int[])p;           // explicit escape: "trust me, this is a whole allocation"
  ```

  The cast is a pure reinterpret (`T*` and `T[]` share a representation) and is greppable in
  review, so the unsafe assertion is always visible. Without it, a view comes *only* from
  `new T[n]`, another `T[]`, or a copy of one - which is what makes "two views alias iff they
  share a base pointer" provable for code that doesn't reach for the escape.
- **Usable as a struct field.** A container can hold a noalias buffer as an `int[]` field:
  store it from `new T[n]` at construction, read it back as a view, and pass it to kernels.
  The store gate guarantees the field can only ever hold a whole-allocation view, so reads
  are sound by construction.
- **You track the length yourself.** A view is thin - it carries no length. Pass the count
  alongside (the C/BLAS convention), keeping the dependency visible in the signature.
- **Allocate with `new T[n]`, free with `delete[_]`.** A view allocated by `new T[n]` is
  released with the array-delete form `delete[_] a;` - note the underscore (`delete[_]`, not
  `delete[]`). The plain `delete a;` form is for a single `new T`, not for an `new T[n]`
  buffer. Free each view exactly once when the kernel is done with it:

  ```c
  double[] a = new double[n];
  // ... use a in kernels ...
  delete[_] a;   // array-delete; underscore is part of the token
  ```

> Not yet supported / deliberately excluded:
> - **Sub-slicing (`a[i:j]`)** is omitted - it would manufacture offset views that overlap,
>   reopening the aliasing problem that the whole-allocation invariant closes.
> - **`T[]` as a generic type argument** (`list<int[]>`) does not parse - `int[]` is a
>   parameter/local/field type, not a container element type (use `int*`).
> - `&a[i]` yields a raw `int*` (a borrow of one element), and the one-way rule applies to
>   it too: it cannot be *implicitly* bound back into a `T[]`. The explicit `(T[])&a[i]` cast
>   is the sanctioned way to re-bless it (this is exactly how `span<T>.chunk` builds an offset
>   tile). The older `union { int* p; int[] v; }` type-pun still works (Fortran `EQUIVALENCE`
>   style) but the explicit cast is preferred - it is shorter and needs no union type.
>   The contract you assert with the cast: **the element ranges actually accessed through the
>   resulting views must be pairwise disjoint** - whole-allocation ownership is not required.
>   Re-blessing two different rows of one row-major matrix (`(double[])&a[i*n]` and
>   `(double[])&a[k*n]`, `i != k`, each indexed `0..n-1`) is sound and is the standard dense
>   linear-algebra pattern; re-blessing two *overlapping* windows and writing through both in
>   the same kernel is the lie that lets the optimizer miscompile.

To return several results from a kernel - a computed array plus auxiliary scalars - return one
`T[]` and pass the rest as `T[]`/`T*` out-parameters. Each `T[]` parameter keeps its noalias
contract, so the vectorizer proves the buffers pairwise disjoint without a runtime overlap
check. See [Returning Multiple Results](LANGUAGE.md#returning-multiple-results) for the full
pattern - it is a general language feature, not HPC-specific.

## Alignment control (`alignas`)

`alignas(N)` over-aligns a type, field, local, global, or heap allocation to `N`
bytes (`N` a power of two, up to 4096). It has two HPC uses: killing false
sharing between cores, and giving the vectorizer aligned buffers.

### Cache-line padding (kill false sharing)

Two cores writing different fields that share a cache line thrash that line back
and forth between their L1s - the "false sharing" cliff. `alignas(64)` on a
struct (or a field) pads it to a full cache line so per-worker slots land on
separate lines:

```c
// Per-worker accumulator: one cache line each, so worker writes never collide.
struct alignas(64) Accum { i64 count = 0; double sum = 0.0; };

Accum[8] slots;                 // stride is 64 - slots[i] and slots[k] never share a line
alignas(64) i64 g_counter = 0;  // a lone hot global on its own line
```

`sizeof` and `alignof` both report the padded size, and array stride follows -
`alignas(64)` on a 12-byte struct makes `sizeof == 64` and `&a[1] - &a[0] == 64`.
This is a layout property of the type: it applies everywhere the type is used
(fields, locals, globals, arrays, `new`).

### Aligned allocation - `new T[n] alignas(N)`

An `alignas(N)` clause on an array `new` over-aligns just that allocation,
without changing the element type's `sizeof` or stride:

```c
double[] a = new double[n] alignas(64);   // base address is a multiple of 64
```

This is an allocation property, not a type property: `sizeof(double)` is still
8 and the buffer strides by 8. The buffer comes from the aligned allocator, so
it must be freed by a path that knows it is over-aligned. Freeing a local it is
bound to - either an explicit `delete[_] a;` / `delete[n] a;`, or the automatic
scope-exit free of the owning `T[]` local - does the right thing. If you instead
store the buffer into a struct field or hand it off elsewhere, delete it
explicitly at that site; the aligned-free tag is tracked on the owning local,
not on arbitrary aliases.

### Assume-aligned for the vectorizer

An aligned `new` also emits an alignment assumption on the returned pointer, so
the optimizer knows the buffer is aligned and `vectorize` loops over it use
aligned vector moves instead of unaligned ones:

```c
double[] a = new double[n] alignas(64);
vectorize for (i64 i = 0; i < n; i = i + 1) { a[i] = a[i] * 2.0 + 1.0; }
delete[_] a;
```

In the emitted IR the allocation carries
`call void @llvm.assume(i1 true) [ "align"(ptr %a, i64 64) ]`, and at `-O2` the
loop's loads and stores become `load <2 x double>, ptr %..., align 64` where the
same loop over an unaligned buffer would show `align 8`. The vectorized code is
correct either way; the aligned form avoids the split-load penalty on the aligned
buffer.

## `span<T>` (noalias) and `view<T>` (may-alias)

`span<T>` and `view<T>` are two-word library wrappers that bundle a buffer pointer with a length, so
a kernel signature carries one argument per buffer instead of a `(ptr, len)` pair. They are the
ergonomic front-ends to the raw `T[]` array-view and the raw `T*` pointer:

| Type | Field | Aliasing | Comes from | Use when |
|------|-------|----------|-----------|----------|
| `span<T>` | `T[] _ptr` | **noalias** - distinct spans address distinct buffers | `new T[n]` or another `T[]` | the buffers are known-disjoint and you want the vectorizer to drop its overlap check |
| `view<T>` | `T*  _ptr` | may-alias - views may overlap | any `T*` | overlapping windows, sub-ranges, general data |

Both are **non-owning**: each wraps a buffer someone else owns (an `array<T>`, a `new T[n]`, a stack
array) and never frees it - the owner must outlive every span/view over it. Import with
`import "span.cb"` / `import "view.cb"`; create one with `wrap(buffer, len)`.

### Getting the noalias guarantee from `span<T>`

Pass a span **by value** (it is two machine words), and inside the kernel bind each span's buffer
into a **local `T[]`** and loop over that:

```c
void axpy(double a, span<double> y, span<double> x)
{
    double[] yv = y.data();    // the noalias view
    double[] xv = x.data();
    i64 n = y.length();
    vectorize for (i64 i = 0; i < n; i = i + 1) { yv[i] = yv[i] + a * xv[i]; }
}
```

At -O2 this vectorizes with **no runtime alias check**: the span's `T[] _ptr` field carries scoped
alias metadata that proves two distinct spans address distinct buffers. Two rules make this work:

- **By value, not by pointer.** A `span<T>*` reloads the buffer pointer and length on every
  iteration (the optimizer cannot prove the struct header is invariant across the loop), so it does
  not vectorize. Pass by value.
- **Index a local `T[]`, not the span.** `span.get`/`set`/`operator[]` are convenient but do *not*
  carry the contract across two spans - those bodies all reach the buffer through the same `this`,
  so the optimizer cannot tell two spans apart. Bind `y.data()` to a local `T[]` and index that.

### Conversions (the one-way lattice)

`span<T> -> view<T>` is allowed via `s.as_view()` - the safe `T[] -> T*` decay that drops the
noalias promise. The reverse, `view<T> -> span<T>`, is a **compile error**: `span.wrap` takes a
`T[]`, so binding a `view`'s raw `T*` into it would forge the noalias contract. A span therefore
only ever originates from a `new T[n]` view or another `T[]`.

### Slicing and partitioning

`view<T>` has `slice(start, end)` returning a sub-range view - sound precisely because views are
may-alias (two sub-views are allowed to overlap). `span<T>.slice(start, end)` exists too, but it
returns a `view<T>`, **not** a `span<T>`: an arbitrary offset cannot keep the whole-allocation noalias
contract (`s.slice(0,10)` and `s.slice(5,10)` overlap while looking distinct), so the sub-range decays
to the may-alias sibling and the **return type itself records that the guarantee was dropped**. There
is deliberately no offset that yields another `span<T>` - that would reopen the aliasing the contract
closes.

What `span<T>` *does* provide is a **disjoint partition** - `chunk(idx, nchunks)` - the data-parallel
primitive (the analog of Rust's `chunks_mut`). It returns the `idx`-th of `nchunks` contiguous,
balanced tiles (the first `length % nchunks` tiles get one extra element, so the tiles cover the span
exactly and differ in length by at most one). Crucially you *cannot spell an overlap*: distinct `idx`
with the same `nchunks` are always disjoint, so each tile keeps the full noalias guarantee. Hand tile
`i` to worker `i` and an -O2 `vectorize` loop over two workers' tiles drops the runtime alias check,
exactly as for a whole span.

```c
// Partition two buffers N ways; each worker gets its own noalias window.
span<double> ys = ...;  // whole buffer
span<double> xs = ...;
span<double> y = ys.chunk(workerId, numWorkers);   // this worker's disjoint tile
span<double> x = xs.chunk(workerId, numWorkers);
axpy(2.0, y, x);                                    // vectorizes, no memcheck
```

Internally `chunk` offsets the noalias `T[]` with the explicit `(T[])&_ptr[start]` escape (since
`T[]` bans pointer arithmetic, `&_ptr[start]` gives a raw `T*` that the cast re-blesses as a view);
it is gated behind the disjoint-tiling API so user code can never forge an overlapping view.

> Self-aliasing a noalias parameter is on the caller (the same caveat as a bare `T[]` parameter):
> passing the same span as two arguments to a *writing* kernel - `axpy(a, y, y)` - is UB the
> optimizer may miscompile. The same applies to tiles: `chunk` is disjoint only for a *fixed*
> `nchunks` - mixing `nchunks` values (`chunk(0,2)` vs `chunk(0,3)`) can overlap. Read-only
> self-aliasing (e.g. a `dot(r, r)` reduction) is harmless.

---

## Across-core data parallelism (`parallel_for_n` / `parallel_reduce`)

`vectorize`, `simd<T,N>`, and `span<T>` parallelize *within* one core (SIMD lanes). `core/hpc/parallel.cb`
adds the *across-core* axis: partition an index range `[0, n)` into balanced sub-ranges and fan the
work out over worker threads. `import "hpc/parallel.cb";`

Each helper comes in two backends:

| Backend | Function | When |
|---------|----------|------|
| Raw threads | `parallel_for_n`, `parallel_reduce<T>` | Occasional region; zero setup (spawns + joins `workers` threads). |
| Thread pool | `parallel_for_n_pool`, `parallel_reduce_pool<T>` | Region runs repeatedly (e.g. per solver timestep); reuses a `ThreadPool`'s resident workers. |

> **Pick the pool variant for anything in a loop.** The two backends look interchangeable at
> the call site, but the raw `parallel_for_n` / `parallel_reduce` **spawn and join `workers` OS
> threads on every call**. That is fine for a one-off region; inside a hot loop (once per solver
> timestep, say) the thread churn dominates and can cost **~7x** versus a resident pool. The rule
> of thumb: *if the region runs more than a handful of times, use `parallel_for_n_pool` /
> `parallel_reduce_pool` with a `ThreadPool` you `init` once* (pin it at construction -
> `pool.init(cores, cpu_mask_lowest(cores))` - see [Pinning workers](#pinning-workers-to-cores-pin)).
> As a safety net, the raw entry points print a one-time advisory to **stderr** once they have
> been called 64+ times - the call pattern that is almost always the per-iteration mistake.

> **Budget the dispatch: a pool fan-out/join is not free either.** One `parallel_for_n_pool`
> call costs roughly **3-5 us per worker** in fan-out plus join (measured ~11 us at 4 workers,
> ~30 us at 8, ~106 us at 20 on a 10C/20T desktop part) - tens of thousands of scalar flops.
> Parallelize a region only when its work clearly dwarfs that: as a rule of thumb, **at least a
> few hundred thousand operations per dispatch**. This bites hardest in *shrinking-work* loops -
> a dense LU / Cholesky trailing update dispatched once per column starts large but ends with a
> handful of rows, where the dispatch dominates and the parallel version runs *slower* than
> serial. Use a work-based cutoff and fall back to the serial loop below it, e.g.
> `if (rows * cols >= 262144) parallel_for_n_pool(...) else for (...)`, rather than
> parallelizing every iteration unconditionally. [`parallel_for_n_dynamic`](#dynamic-scheduling-parallel_for_n_dynamic--parallel_for_n_dynamic_pool)
> is also worth trying here instead of a fixed cutoff: since a shrinking trailing update is
> exactly the skewed-cost shape dynamic scheduling targets, it can let the last few columns
> keep parallelizing productively instead of falling back to serial.

`workers <= 0` selects a default (`hardware_concurrency()`, or the pool's worker count for the pool
variants); `workers` is clamped to `n`. The worker body is a **capturing** lambda over its `[lo, hi)`
range: CFlat lambdas capture enclosing locals by value (primitives, `simd<T,N>`, pointers, and
`double[]` array-views copy by value; named struct locals capture by reference), so the kernel reaches
its buffers and scalars directly - no context struct and no `void*` cast. All four helpers join/wait
before returning, so the captured environment (which lives in the caller's frame) outlives every worker
that runs it.

### `parallel_for_n` - parallel map

```c
double alpha = 2.5;
parallel_for_n(n, 0, (i64 lo, i64 hi) => {               // 0 -> hardware_concurrency()
    for (i64 i = lo; i < hi; i = i + 1) { y[i] = y[i] + alpha * x[i]; }   // y, x, alpha captured
});
```

The body is called once per worker (not once per element), so there is no per-element call overhead.
A `void`-returning body lambda can be written inline as shown.

### `parallel_reduce<T>` - parallel reduction

Each worker folds its chunk into a partial `T`; the helper merges the partials with `combine` (seeded
by `identity`) after all workers finish. Reductions are exactly the kernels that *cannot* use
`vectorize` for the cross-element accumulation (FP reassociation changes the result), so threads are
their only coarse parallelism - while the per-chunk `partial` loop can still be a `vectorize`/`simd`
reduction, stacking SIMD under threads.

An inline value-returning lambda passed *directly* as an argument does not pick up its return type, so
the capturing `partial` is built as a typed local lambda first; `combine` needs no capture and stays a
plain named function:

```c
double addD(double x, double y) { return x + y; }       // combine (associative, no capture)

Lambda<double(i64, i64)> partial = (i64 lo, i64 hi) => {       // captures a, b -> Lambda<...>
    double s = 0.0;
    for (i64 i = lo; i < hi; i = i + 1) { s = s + a[i] * b[i]; }   // a, b captured
    return s;
};
double dot = parallel_reduce<double>(n, 4, 0.0, partial, addD);
```

`combine` must be associative. Because both SIMD lane-reduction *and* the per-worker split reassociate
floating-point adds, the result depends on `workers` and is **not** bit-identical to a serial fold -
standard and expected for parallel/SIMD reductions.

For an **integer** reduction (a count, a histogram bin, a checksum) `T` is `i64` and `identity` is the
zero literal cast to the element type - `(i64)0`, not `0` (an unannotated `0` is `int`, which will not
match the `i64` reduction):

```c
i64 addI(i64 x, i64 y) { return x + y; }                 // combine
i64 count = parallel_reduce<i64>(n, 0, (i64)0, countPartial, addI);
```

### Pinning workers to cores (`pin`)

The raw-thread `parallel_for_n` and `parallel_reduce<T>` take a trailing `bool pin = false`. Set it to
`true` to pin worker `w` to core `w % hardware_concurrency()` (via `SetThreadAffinityMask`), which
removes the OS scheduling jitter that otherwise caps speedup on compute-bound kernels - the single most
important knob for steady throughput (see [THREADING.md](THREADING.md)):

```c
i64 count = parallel_reduce<i64>(n, 0, (i64)0, partial, addI, /*pin=*/true);
```

Leave it `false` when the region oversubscribes the machine or shares cores with other work.

The `*_pool` variants take no per-call `pin`, and deliberately so: a pool task doesn't choose which
worker runs it, so affinity has to live on the resident worker, not the submission. Pin the pool's
workers **once** at construction via the second `init` argument:

```c
ThreadPool pool;
pool.init(8, cpu_mask_lowest(8));   // 8 workers, worker i pinned to core i
// ... then call parallel_for_n_pool(&pool, ...) once per timestep; workers stay pinned.
```

`init(int workers, u64 pinMask)` pins worker `i` to a single core - the `i`-th set bit of `pinMask`
(see `__threadpool_nth_core`) - so a pool can be pinned to an arbitrary core subset, not just the low
cores. `cpu_mask_lowest(n)` builds the mask for cores `0..n-1` (the usual one-worker-per-core layout);
pass any bitmask for a custom set. The plain `init(workers)` overload leaves the workers unpinned. This
closes the old pin-vs-pool gap: a bulk-synchronous solver now gets thread reuse **and** affinity at the
same time.

`cpu_mask_lowest(n)` assumes cores `0..n-1` are the right ones to use, which is not true on a
heterogeneous or multi-CCX machine - core 0 and core 1 might be on different cache domains, or one
might be a slower efficiency core. `import "topology.cb";` gives mask constructors that understand the
actual machine, built on a memoized `CpuTopology` snapshot (`cpu_topology()`):

- `cpu_mask_physical(n)` - one logical CPU per physical core (skips SMT siblings), for the first `n`
  cores in enumeration order.
- `cpu_mask_perf_cores()` / `cpu_mask_efficiency_cores()` - all logical CPUs of the performance-class
  cores, and of everything below that class, split by the OS-reported `EfficiencyClass`. On a uniform
  machine `cpu_mask_efficiency_cores()` is `0` and `cpu_mask_perf_cores()` covers everything - that is
  correct-by-definition, not a bug.
- `cpu_mask_llc(domain)` / `cpu_llc_count()` - the logical CPUs sharing LLC (L3 cache / CCX) domain
  `domain`, and how many domains exist. This is the discriminating tool when a machine does not expose
  distinct efficiency classes but still has separate cache domains (e.g. two same-class CCXs).
- `cpu_mask_numa(node)` / `cpu_numa_count()` - the NUMA-node equivalent.

Worked example: pin a pool to stay inside a single LLC/CCX domain, so every worker shares one L3 cache
and cross-core communication never crosses the slow inter-CCX link:

```c
import "topology.cb";
import "threadpool.cb";

u64 llc0 = cpu_mask_llc(0);
int workers = 0;
u64 m = llc0;
while (m != (u64)0) { m = m & (m - (u64)1); workers = workers + 1; }   // popcount(llc0)

ThreadPool pool;
pool.init(workers, llc0);
// ... parallel_for_n_pool(&pool, ...) - every worker stays inside LLC domain 0.
```

`workers` here is the popcount of the mask (one worker per logical CPU in that domain); pass a smaller
count if you want fewer workers than the domain has room for - `init` still only pins to bits within
`llc0`. On a heterogeneous box (AMD
Strix Point: 4 Zen5 performance cores + 6 Zen5c compact cores on two separate CCXs) `cpu_mask_llc(0)` and
`cpu_mask_llc(1)` isolate each CCX; `cpu_mask_perf_cores()` isolates the 4 Zen5 cores directly since
Windows reports them at a higher `EfficiencyClass` than the Zen5c cores.

Limits: masks are a bare `u64`, so only the first 64 logical processors in Windows processor group 0 are
covered (multi-group machines and >64 logical processors are out of scope, same implicit limit
`cpu_mask_lowest` already had). Any query failure on any platform degrades to a uniform synthetic
snapshot (`physicalCount == logicalCount`, one LLC domain, one NUMA node) rather than to a zero mask,
so a pinned pool never silently un-pins.

`topology.cb` does a real walk on all three platforms:

| Platform | Topology source | Pinning |
|----------|-----------------|---------|
| Windows | `GetLogicalProcessorInformationEx(RelationAll)` | `SetThreadAffinityMask` |
| Linux | sysfs: `cpu/online`, `topology/thread_siblings_list` (collapses SMT), `cache/indexN` (highest level >= 2 -> LLC), `node/nodeN/cpulist`; `EfficiencyClass` is approximated from ARM `cpu_capacity` rank, else Intel `cpu_core`/`cpu_atom` | `pthread_setaffinity_np` |
| macOS | `sysctlbyname` over `hw.physicalcpu` / `hw.logicalcpu` / `hw.nperflevels` / `hw.perflevelN.*`; perflevel 0 is the most performant, so `coreEff = nperflevels - 1 - level`; LLC domains are each perflevel's `cpusperl2` cluster | **none** |

**macOS has no CPU affinity API.** Darwin exposes no `sched_setaffinity`, and `thread_affinity_policy` is
an advisory L2-sharing hint that returns `KERN_NOT_SUPPORTED` on Apple Silicon. `os.thread_set_affinity`
is therefore an honest no-op there, and `os.thread_affinity_supported()` returns `false` so callers can
branch. The macOS mask bits are **synthetic and informational**: they are assigned in perflevel order and
do not correspond to any OS-visible CPU number.

The actionable macOS lever is **QoS class**, which is what actually steers a thread onto the P or E
cluster. `cpu_qos_for_mask(mask)` maps a mask to the QoS class that best expresses it - a mask contained
in `cpu_mask_efficiency_cores()` maps to `QOS_CLASS_BACKGROUND`, one contained in `cpu_mask_perf_cores()`
maps to `QOS_CLASS_USER_INTERACTIVE`, and anything else (including *every* mask on a uniform,
single-core-class machine, where there is no P/E split to express) maps to `0` / unspecified. `ThreadPool`
applies this automatically: when `pinMask` is set, each worker calls `os.thread_set_qos_self()` on entry.
QoS must be set from the target thread itself, which is why it is a self-only call rather than a
handle-taking one.

### NUMA domains (`core/numa.cb`)

`cpu_mask_numa`/`cpu_mask_llc` pin a pool *inside* a memory domain; `core/numa.cb` closes the
other half of the story - getting the *data* into that domain too. On a multi-socket (or
multi-die) box, a core reading memory that lives on a **remote** node pays extra interconnect
latency on every access - often 1.5-2x a local access - and the usual way this bites a program is
silent: a single thread loads a big buffer at startup, the OS's first-touch page-fault policy
places every page on *that* thread's node, and every other node's cores then pay the remote
penalty for the buffer's entire lifetime, with no error and no obvious symptom besides "this
scales worse than it should." `import "numa.cb";` gives you the domain-aware primitives to place
both the memory and the threads deliberately, so first-touch never gets the chance to place a
page wrong.

**Query API** (`core/topology.cb`, alongside `cpu_mask_numa`): `cpu_numa_count()` is the existing
domain count; `numa_domain_id(index)` / `numa_domain_index(id)` convert between a snapshot slot
and the OS's own node ID (the acquisition API below is ID-keyed, since that's what a caller holds
onto). `numa_domain_info(id, &out)` fills a `NumaDomainInfo` with that node's core counts (from
the memoized topology snapshot) and its **live** free/total memory (queried fresh on every call,
since free memory changes; never memoized):

```c
struct NumaDomainInfo {
    int id             = -1;
    int physicalCores  = 0;   // physical cores whose coreMask intersects the node
    int logicalCpus    = 0;   // popcount(cpuMask)
    u64 cpuMask        = 0;
    i64 memTotalBytes  = -1;  // -1 = the OS does not expose a per-node total
    i64 memFreeBytes   = -1;  // -1 = unknown
};
bool numa_domain_info(int id, NumaDomainInfo* out);   // false if `id` is unknown
```

`-1` means "the OS will not tell us this", not "zero": Windows has no documented per-node
**total**-memory API (`memTotalBytes` is always `-1` there; `memFreeBytes` is real, from
`GetNumaAvailableMemoryNodeEx`); Linux reports both real, from `/sys/devices/system/node/nodeN/
meminfo`; macOS reports a real **total** for its single synthetic domain (`hw.memsize`) but
`memFreeBytes` is always `-1` (not worth binding `host_statistics64` for a tier that has no
placement lever anyway - see the table below).

**The `NumaDomain` class** is the acquisition layer - one domain object hands out pinned threads
and node-local memory from the *same* node, so the correct HPC pattern (data and the cores that
touch it live together) is the path of least resistance:

```c
NumaDomain* d = NumaDomain.acquire(numa_domain_id(0), NUMA_ISOLATE_PROCESS);
// ... d->acquireThread / d->allocLocal ...
d->release(false);   // or just let d go out of scope - ~NumaDomain calls release(false)
```

- **`NumaDomain.acquire(id, required)`** reserves domain `id` in-process (a module-level registry
  rejects double-acquire) and returns `nullptr` on exactly three failures: `id` is not a known OS
  node ID, `id` is already acquired by this process, or `required == NUMA_ISOLATE_PROCESS` on
  macOS (no affinity API exists there to achieve it). There are two confinement tiers, weakest to
  strongest:

  ```c
  const int NUMA_ISOLATE_NONE    = 0;  // bookkeeping only - domain object, thread hand-out,
                                        // allocLocal; no scheduler interference
  const int NUMA_ISOLATE_PROCESS = 1;  // + this process's UN-PINNED threads are steered OFF the
                                        // acquired domain; threads handed out BY the domain are
                                        // pinned inside it regardless
  ```

  `NUMA_ISOLATE_NONE` always succeeds for a valid, unacquired ID (it's the macOS tier: threads get
  QoS steering instead of pinning, `allocLocal` is a plain unbound allocation). `NUMA_ISOLATE_PROCESS`
  is best-effort even where it "succeeds": on a single-domain machine the complement of every
  acquired domain's mask is empty, so the confinement step is silently skipped (the addendum case)
  and pinning is the only protection that remains - `level` still reports `NUMA_ISOLATE_PROCESS`
  because the pinning half of the contract holds.
- **`release(killRemaining)`** returns the domain to the registry and restores this process's
  saved CPU affinity/CPU-set state (recomputed so releasing one domain doesn't disturb another
  domain's confinement). `killRemaining` kills any `NumaThread`s still running (see below);
  passing `false` lets stragglers finish on their own, detached. `~NumaDomain` calls
  `release(false)` on scope exit, so a `NumaDomain*` local auto-releases.
- **`acquireThread(entry, arg)`** hands out one thread pinned to a free **physical** core of the
  domain (the lowest SMT sibling - a domain with N physical cores supports exactly N concurrent
  `NumaThread`s; SMT pairs are never handed out as separate cores) and returns `nullptr` once every
  physical core is taken. `entry` is a plain non-capturing `function<void(void*)>`, same shape as
  `Thread.start`/`ThreadPool`'s task functions. The thread's *own* future allocations are steered to
  the node too, where the OS has the lever: Linux calls `set_mempolicy(MPOL_BIND)` on itself inside
  the trampoline (self-only, hence thread-side rather than `acquire`-side); Windows has no per-thread
  mempolicy, so pinning plus first-touch is the mechanism, and `allocLocal` is the explicit lever for
  a pre-sized buffer; macOS does neither (QoS steering only).
- **`releaseThread(t)`** returns a handed-out thread's core to the free pool, killing it if it's
  still running (`os.thread_kill` - the same forced-teardown caveats as everywhere else in this repo:
  synchronous and safe to clean up after on Windows, deferred and packet-leaking on POSIX). Prefer
  `t->join(timeoutMs)` first for a graceful return; `releaseThread` is the hard path for a straggler.
  There is no infinite-timeout sentinel that's portable across platforms - Windows' `join` maps a
  negative timeout to `WaitForSingleObject`'s `INFINITE`, but the macOS/Linux polling join treats any
  negative value as "already timed out" - so a thread that may run indefinitely is joined via a retry
  loop (`while (!t->join(1000)) { }`), not a single call with a sentinel timeout.
- **`allocLocal(size)` / `freeLocal(p)`** give node-bound pages (`VirtualAllocExNuma` on Windows,
  `mmap`+`mbind(MPOL_BIND)` on Linux, a plain allocation on macOS), 64K-aligned and zeroed, freed
  symmetrically.
- **`info()`** is a live re-query of `numa_domain_info` for this domain's ID - the same struct as
  above, fetched through the object you already have.

**Per-OS capability table** (single-process scope only - this library places and pins *this*
process's own memory and threads; it does not evict or isolate other processes):

| Capability | Windows | Linux | macOS |
|---|---|---|---|
| Pin a `NumaThread` to a physical core | real (`SetThreadAffinityMask`) | real (`pthread_setaffinity_np`) | none - QoS steering only |
| Keep un-pinned process threads off an acquired domain (`NUMA_ISOLATE_PROCESS`) | real, via CPU Sets (`SetProcessDefaultCpuSets`) - process *affinity* is deliberately never touched, since a thread's affinity must be a subset of the process's, and shrinking it would break pinning a `NumaThread` inside the very domain being confined | real, via `sched_setaffinity(0, ...)` on the calling thread (the confining thread; inherited by threads spawned after) | not achievable - `acquire(id, NUMA_ISOLATE_PROCESS)` returns `nullptr` |
| Bind a thread's own future allocations to the node | pin + first-touch (no per-thread mempolicy API) | `set_mempolicy(MPOL_BIND)`, self-only, applied in the trampoline | none |
| `allocLocal` - explicit node-bound buffer | real (`VirtualAllocExNuma`) | real (`mmap` + `mbind`) | plain unbound allocation (no per-node lever) |
| Query free / total memory per node | free real, total always `-1` (no documented user-mode API) | both real (`/sys/.../nodeN/meminfo`) | total real for the one synthetic domain (`hw.memsize`), free always `-1` |

**Worked example - loading a very large file striped across domains.** This is the pattern the
class exists to make easy: query each domain's free memory to size a proportional shard, place
each shard's buffer with `allocLocal` (so the pages are born on the right node - no first-touch
discipline needed for the buffer itself), give each domain one pinned I/O thread to load its own
shard, then hand every physical core in every domain a compute worker that touches only its own
domain's data:

```c
import "numa.cb";
import "topology.cb";
import "filesystem.cb";

// Shared read-only input path for every domain's loader thread. A
// function<void(void*)> entry is non-capturing, so a plain global is how
// workers reach data every domain needs (the per-shard data itself still
// travels through the arg pointer).
string g_loadPath = default;

struct FileShard {
    NumaDomain* domain = nullptr;   // owns the pages + the cores
    i8*  base   = nullptr;          // domain-local buffer (allocLocal)
    i64  offset = 0;                // byte range of the file in this shard
    i64  len    = 0;
};

// Reads this shard's [offset, offset+len) range of g_loadPath straight into
// its domain-local buffer, in 64 MB chunks (File.seek/readBytes are int-sized,
// so a multi-GB shard is read incrementally rather than in one call).
void loadShard(void* arg) {
    FileShard* s = (FileShard*)arg;
    File f;
    f.openRead(g_loadPath);
    i64 chunkSize = (i64)1 << 26;
    i64 done = 0;
    while (done < s->len) {
        i64 remaining = s->len - done;
        int n = (remaining < chunkSize) ? (int)remaining : (int)chunkSize;
        f.seek((int)(s->offset + done), 0);
        f.readBytes((i8*)(s->base + done), n);
        done = done + (i64)n;
    }
    f.close();
}

// One compute worker's context: which shard, which sub-range of its cores.
struct ComputeCtx {
    FileShard* shard       = nullptr;
    int        workerIdx   = 0;
    int        workerCount = 0;
};

// Touches ONLY this worker's slice of ITS domain's shard - every read is
// node-local. (Stub body: replace with the real per-byte kernel.)
void computeWorker(void* arg) {
    ComputeCtx* c = (ComputeCtx*)arg;
    FileShard* s = c->shard;
    i64 lo = s->len * (i64)c->workerIdx / (i64)c->workerCount;
    i64 hi = s->len * (i64)(c->workerIdx + 1) / (i64)c->workerCount;
    i64 i = lo;
    i64 checksum = 0;
    while (i < hi) { checksum = checksum + (i64)s->base[i]; i = i + 1; }
    // ... consume checksum (e.g. fold into a shared atomic total) ...
}

// join() has no portable "wait forever" sentinel (see the acquireThread note
// above), so a long-running thread is joined via a retry loop.
void joinFully(NumaThread* t) {
    if (t == nullptr) { return; }
    while (!t->join(1000)) { }
}

int main() {
    g_loadPath = "bigfile.bin";   // set before any domain thread is spawned

    int shardCount = cpu_numa_count();
    FileShard[8] shards = default;

    File probe;
    probe.openRead(g_loadPath);
    i64 fileSize = (i64)probe.size();
    probe.close();

    // 1) Query each domain (feature 2) and acquire it (feature 3). Sizing is
    //    proportional to each domain's free local memory, so an asymmetric
    //    machine (or a half-full node) does not overcommit one node while
    //    another sits empty. Single-domain machine: loop runs once, everything
    //    below still works (addendum path).
    i64 freeTotal = 0;
    int i = 0;
    while (i < shardCount) {
        NumaDomainInfo inf = default;
        numa_domain_info(numa_domain_id(i), &inf);
        if (inf.memFreeBytes > 0) { freeTotal = freeTotal + inf.memFreeBytes; }
        i = i + 1;
    }

    i64 covered = 0;
    i = 0;
    while (i < shardCount) {
        shards[i].domain = NumaDomain.acquire(numa_domain_id(i), NUMA_ISOLATE_PROCESS);
        NumaDomainInfo inf = shards[i].domain->info();
        i64 slice = 0;
        if (i == shardCount - 1 || freeTotal <= 0) {
            slice = fileSize - covered;
        } else {
            slice = fileSize * inf.memFreeBytes / freeTotal;
        }
        shards[i].offset = covered;
        shards[i].len    = slice;
        // 2) The pages are BORN on the right node: allocLocal binds them
        //    (VirtualAllocExNuma / mbind) - no first-touch discipline needed
        //    for the buffer itself.
        shards[i].base = (i8*)shards[i].domain->allocLocal(slice);
        covered = covered + slice;
        i = i + 1;
    }

    // 3) Parallel load (feature 5): each domain contributes ONE pinned I/O
    //    thread that reads its own [offset, offset+len) range straight into
    //    its domain-local buffer.
    NumaThread*[8] loaders = default;
    i = 0;
    while (i < shardCount) {
        loaders[i] = shards[i].domain->acquireThread(loadShard, (void*)&shards[i]);
        i = i + 1;
    }
    i = 0;
    while (i < shardCount) { joinFully(loaders[i]); i = i + 1; }

    // 4) Compute (features 5/6): every physical core of every domain gets a
    //    worker; each worker's slice comes from ITS domain's shard, so all
    //    hot reads are node-local.
    NumaThread*[64] workers = default;
    ComputeCtx*[64] ctxs    = default;
    int nWorkers = 0;
    i = 0;
    while (i < shardCount) {
        NumaDomainInfo inf = shards[i].domain->info();
        int w = 0;
        while (w < inf.physicalCores) {
            ComputeCtx* ctx = new ComputeCtx;
            ctx->shard       = &shards[i];
            ctx->workerIdx   = w;
            ctx->workerCount = inf.physicalCores;
            ctxs[nWorkers]    = ctx;
            workers[nWorkers] = shards[i].domain->acquireThread(computeWorker, (void*)ctx);
            nWorkers = nWorkers + 1;
            w = w + 1;
        }
        i = i + 1;
    }
    i = 0;
    while (i < nWorkers) {
        joinFully(workers[i]);
        delete ctxs[i];
        i = i + 1;
    }

    // 5) Release (features 4/6): free the domain-local buffers, then release
    //    every domain - restores process affinity/CPU sets.
    i = 0;
    while (i < shardCount) {
        shards[i].domain->freeLocal(shards[i].base);
        shards[i].domain->release(false);   // graceful; release(true) would kill stragglers
        i = i + 1;
    }
    return 0;
}
```

What the library did for the user, step by step: domain discovery (count + IDs), free-memory-
proportional shard sizing (`info()`), buffers placed on the right node at allocation time
(`allocLocal`), loader and compute threads pinned to cores of the *same* node as their data
(`acquireThread`), un-pinned threads of the rest of the process kept off the acquired domains
(`NUMA_ISOLATE_PROCESS`), and a single release path that restores process affinity and can reap
stragglers. On a 1-domain box the identical code degrades to "pinned threads + one big buffer" -
`shardCount == 1`, the free-memory proportion is trivially `1.0`, and every step still runs.

**Honest limitations:**

- **Single-process scope, always.** The library places and pins only the calling process's own
  threads and memory; it never evicts, isolates, or even looks at other processes. The OS
  scheduler remains free to run other processes' threads on an acquired domain's cores - per-thread
  pinning plus the process-affinity/CPU-Set complement is the *entire* confinement story, not a
  system-wide guarantee.
- **Confinement is best-effort, not enforced.** `NUMA_ISOLATE_PROCESS` silently skips the
  confinement step when the complement is empty (the single-domain case), and on Linux
  `sched_setaffinity(0, ...)` only affects the calling thread - pre-existing threads created
  *before* `acquire()` are never chased down and re-pinned (enumerating and rewriting foreign
  threads' affinity was judged too intrusive). In practice this covers the normal HPC shape:
  acquire domains at startup, before spawning anything else.
- **The existing 64-CPU / processor-group-0 limit applies here too.** Masks are a bare `u64`; a
  NUMA node whose CPUs live outside Windows processor group 0 (>64 logical processors on the
  machine) reports `cpuMask == 0` even though `numa_domain_info` still returns valid ID and memory
  fields. Lifting this is its own plan, same as the rest of `topology.cb`.
- **No page migration in v1.** Pages are placed correctly at allocation time (`allocLocal`) or via
  first-touch after pinning; nothing in this library *moves* an already-placed page afterward. A
  `moveToDomain(p, size, domain)` helper is a recorded post-v1 TODO (Linux can do this
  unprivileged via `mbind(MPOL_MF_MOVE)`; Windows has no migration API, so the portable shape
  would alloc-on-target + copy + free, changing the pointer) - see
  `internal/plan/numa-domains.md` for the exact mechanism.
- **Multi-node performance itself is unverified on this repo's hardware.** Every API call
  (query, acquire, confinement, pinning, `allocLocal`, kill/release) was validated for
  correctness on Windows and on Linux/WSL, but the only development machine available is a
  single-NUMA-node box, so the actual latency win of keeping data and compute on the same node
  has not been - and could not be - measured here. The design keeps every multi-node decision
  data-driven off the topology snapshot (nothing is hardcoded to "2 domains"), so a real
  multi-socket machine exercises the same code path without any change.

### Per-worker floating-point environment (trap NaN/Inf, flush denormals)

A numerical blow-up to `NaN`/`Inf` is otherwise silent: a quiet `NaN` propagates from the step that
produced it to wherever it first becomes visible (a garbage value many steps later), with no signal at
the source. Each parallel worker can opt into a different per-thread FP environment so the fault lands
where it is born, or so denormals stop dragging the hot loop into the slow path.

The raw `parallel_for_n` and `parallel_reduce<T>` take a trailing `int fpConfig = 0` after `pin`; the
`*_pool` variants inherit the config their `ThreadPool` was `init`'d with (the third `init` argument):

```c
// Trap on the first NaN/Inf in any worker - faults at the producing instruction.
parallel_for_n(n, workers, body, /*pin=*/true, FP_TRAP_DEFAULT);

// Flush denormals to zero across a pinned pool (throughput knob).
ThreadPool pool;
pool.init(8, cpu_mask_lowest(8), FP_FLUSH_DENORMALS);
```

`FP_TRAP_DEFAULT` is the "catch NaN/Inf at the source" diagnostic; `FP_FLUSH_DENORMALS` is the
throughput knob (denormals can cost 10-100x). The setting is per-thread, applied at worker creation,
and never touches the main thread. See [Per-thread floating-point environment](THREADING.md#per-thread-floating-point-environment-corethreadcb)
in THREADING.md for the full constant list, the `fp_enable_traps`/`fp_disable_traps` in-thread helpers,
and the `program._fpConfig` field.

### Per-thread RNG for parallel Monte Carlo

A parallel Monte Carlo reduction needs each worker to draw from a **statistically independent** stream;
sharing one `Random` across threads is both a data race and statistically wrong. `core/random.cb`'s
splitmix64 gives two exact ways to carve independent substreams (see the comments in `random.cb`):

- `jump(i64 n)` - fast-forward a stream by `n` draws in O(1). When you know the per-worker draw count,
  seed one generator, then jump worker `i` to its window so the windows cannot overlap.
- `split()` - return a fresh generator seeded from this one's next output (SplittableRandom). Use it
  when the worker/task count is not known up front.

Because the kernel body is called once per worker with its `[lo, hi)` range, derive the worker's stream
from `lo` inside `partial` - no shared state, no synchronization:

```c
import "random.cb";

i64 drawsPerSample = 2;                // 2 draws (x, y) per sample

Lambda<i64(i64, i64)> piPartial = (i64 lo, i64 hi) => {        // captures drawsPerSample -> Lambda<...>
    Random rng = default;
    rng.seed(0x1234567);                   // same base seed on every worker...
    rng.jump(lo * drawsPerSample);         // ...then jump to this chunk's window (no overlap)
    i64 inside = 0;
    for (i64 i = lo; i < hi; i = i + 1) {
        double x = rng.nextDouble();
        double y = rng.nextDouble();
        if (x * x + y * y <= 1.0) { inside = inside + 1; }
    }
    return inside;
};
// pi ~= 4 * parallel_reduce<i64>(n, 0, (i64)0, piPartial, addI, true) / n
```

### Pool variants

The pool variants take a `ThreadPool*` first argument and otherwise match. Amortize the worker threads
across a hot loop:

```c
ThreadPool pool;  pool.init(4);
for (int step = 0; step < 1000; step++) {
    double r = parallel_reduce_pool<double>(&pool, n, 0, 0.0, partial, addD);
    // ... use r ...
}
pool.shutdown();
```

> The pool variants are named `*_pool` rather than overloading the raw names: two generic functions of
> the same name monomorphize to the same mangled symbol and would collide, so both pool entry points
> carry the suffix for a uniform API.

### Dynamic scheduling (`parallel_for_n_dynamic` / `parallel_for_n_dynamic_pool`)

`parallel_for_n` and `parallel_for_n_pool` use **static** scheduling: `[0, n)` is split once, up
front, into exactly `workers` contiguous near-equal ranges. That is optimal when cost-per-index is
uniform (axpy, stencil) - zero coordination, perfect balance by construction. It falls over when
cost varies with the index - a triangular loop (iteration `i` costs `~n-i`), CSR rows with a skewed
nonzero distribution, or a shrinking trailing update (see the dispatch-budget callout above) - because
the *unluckiest* worker's range sets the wall time. With 4 workers and a triangular cost, the first
quarter of the range holds ~44% of the total work: ~2.3x speedup instead of ~4x.

`parallel_for_n_dynamic` / `parallel_for_n_dynamic_pool` fix this by cutting `[0, n)` into many small
chunks - far more chunks than workers - and letting each worker pull the next chunk from a shared
counter as soon as it finishes its current one:

```c
void parallel_for_n_dynamic(i64 n, int workers, i64 chunk,
                            Lambda<void(i64, i64)> body,
                            bool pin = false, int fpConfig = 0);

void parallel_for_n_dynamic_pool(ThreadPool* pool, i64 n, int workers,
                                 i64 chunk, Lambda<void(i64, i64)> body);
```

Same body shape as `parallel_for_n` - the lambda still receives `[lo, hi)`, so a kernel migrates by
changing one call name and adding the `chunk` argument:

```c
parallel_for_n_dynamic(n, 0, 0, (i64 lo, i64 hi) => {   // workers=0 -> hardware_concurrency(); chunk=0 -> auto
    for (i64 i = lo; i < hi; i = i + 1) { out[i] = triangularWork(i); }
});
```

A worker that draws cheap chunks just draws more of them; balance is automatic for any cost skew,
and no worker idles until the last chunk is claimed. Every index is still processed **exactly once**
by exactly one worker, so a race-free body produces results **identical to serial** - chunking only
changes *which thread* computes an index, never the per-index computation or its floating-point
order. That is what makes `parallel_for_n_dynamic` a safe drop-in replacement for `parallel_for_n`.

> **Static vs. dynamic - pick by cost shape, not by default.**
>
> | | Static (`parallel_for_n[_pool]`) | Dynamic (`parallel_for_n_dynamic[_pool]`) |
> |---|---|---|
> | Coordination | None after the initial split | One relaxed atomic fetch-add per chunk, on a shared, contended counter line |
> | Partition | Deterministic, contiguous per-worker ranges (cache/first-touch friendly) | Non-deterministic - which worker computes which chunk varies run to run |
> | Imbalance | Eats the full skew (unluckiest worker sets wall time) | Bounded by roughly one chunk's worth of work |
> | Best for | Uniform cost-per-index (axpy, stencil, dense BLAS) | Skewed cost-per-index (triangular loops, ragged CSR rows, shrinking trailing updates) |
>
> Rule of thumb: uniform body -> static; skewed body -> dynamic. Reach for dynamic only when the
> skew is real - on a uniform body the extra atomic traffic buys nothing over static's free split.

`chunk <= 0` selects an automatic size: `max(1, n / (workers * PFOR_DYN_CHUNKS_PER_WORKER))`, clamped
to `n`. `PFOR_DYN_CHUNKS_PER_WORKER` (currently 16) targets ~16 chunks per worker, which bounds
imbalance at roughly 1/16 of one worker's share while still amortizing the atomic + dispatch cost over
a meaningful range; it is a named global so it can be retuned in one place after measurement. Pass an
explicit `chunk` to tune further: smaller for bodies with extreme per-index cost (tighter balance, more
counter traffic), larger for very cheap bodies (amortize the counter line ping-pong). `workers <= 0` and
`workers` clamped to `n` behave exactly as in the static helpers; `n <= 0` is a no-op, exactly as in the
static helpers.

The pool variant's refused-submit fallback differs from the static pool's: since every worker (and any
refused submission) shares the *same* counter, a refused task drains the counter directly on the calling
thread instead of running one fixed range - it just claims whatever chunks are still unclaimed, so it
composes correctly with chunks concurrently pulled by workers that *did* get submitted.

#### `parallel_reduce_dynamic<T>` / `parallel_reduce_dynamic_pool<T>` - dynamic reduction

The same chunk-pulling scheduler applies to reductions, for the same reason: `parallel_reduce<T>` /
`parallel_reduce_pool<T>` split `[0, n)` once, up front, so a skewed `partial` (e.g. the triangular-cost
kernel above, adapted to return a value instead of writing an array) imbalances the same way a skewed
`parallel_for_n` body does.

```c
T parallel_reduce_dynamic<T>(i64 n, int workers, i64 chunk, T identity,
                             Lambda<T(i64, i64)> partial,
                             Lambda<T(T, T)> combine, bool pin = false, int fpConfig = 0);

T parallel_reduce_dynamic_pool<T>(ThreadPool* pool, i64 n, int workers, i64 chunk, T identity,
                                  Lambda<T(i64, i64)> partial,
                                  Lambda<T(T, T)> combine);
```

Each worker seeds **one** local accumulator with `identity`, folds every chunk it pulls via
`combine(acc, partial(lo, hi))`, and writes that accumulator to its own partials slot once it exhausts
the counter (a slot per worker, never shared - no synchronization needed on the write). After all
workers finish, the helper merges the `workers` slots exactly like the static `parallel_reduce<T>` does.
`chunk <= 0` uses the same automatic sizing as `parallel_for_n_dynamic` (`__pfor_dyn_auto_chunk`,
`PFOR_DYN_CHUNKS_PER_WORKER`); `workers <= 0` and clamping to `n` behave exactly as in the static
reduction; `n <= 0` returns `identity` untouched, without calling `partial` or `combine`. `combine` must
still be associative, exactly as for `parallel_reduce<T>`.

```c
Lambda<double(i64, i64)> partial = (i64 lo, i64 hi) => {
    double s = 0.0;
    for (i64 i = lo; i < hi; i = i + 1) { s = s + triangularCost(i); }
    return s;
};
double total = parallel_reduce_dynamic<double>(n, 0, 0, 0.0, partial, addDouble);
```

> **Reproducibility caveat - read this before reaching for the dynamic reduction.**
>
> `parallel_for_n_dynamic` is a safe drop-in for `parallel_for_n` because every index still runs exactly
> once, on exactly one worker - chunking only changes *which thread* computes an index, never any
> per-index computation. A reduction is different: `parallel_reduce_dynamic<T>` folds *multiple* chunks
> together per worker, and which chunks a given worker happens to pull - and therefore the order
> `combine` sees them in - varies **run to run**, even at a fixed `workers` count. For an exactly
> associative `combine` (integer add, min/max, bitwise ops) the result is bit-identical regardless of
> order, so this is a non-issue. For a floating-point `combine`, reassociation changes rounding, so the
> result is **not** reproducible run-to-run - strictly weaker than the static `parallel_reduce<T>`, whose
> partition (and therefore its reassociation) is deterministic for a fixed `workers` count. If your
> reduction must be bit-stable across runs (regression baselines, checkpoint/replay determinism), stay on
> `parallel_reduce<T>` regardless of cost skew; reach for the dynamic variant only when the skew is real
> and run-to-run FP drift (typically far below any meaningful tolerance) is acceptable.

---

## Huge pages

`import "vmem.cb";` gives an opt-in way to back a big buffer with 2 MB pages
instead of the usual 4 KB, at the `vmem_alloc_*` tier only - `block_allocator`
/ `bucket_allocator` / `page_pool` and friends keep handing out 64 KB pages,
since backing those with 2 MB pages would waste memory for no TLB win. Reach
for huge pages when a kernel streams or randomly accesses a multi-GB buffer
and dTLB misses (page-walk cost on every access once the buffer's page count
blows past the CPU's dTLB entry count) are the bottleneck - see
`performance/hpc/hugepage_bench.cb` for a worked dependent-pointer-chase
benchmark that isolates the effect.

```c
import "vmem.cb";

bool avail = vmem_huge_pages_available();   // pre-flight query
void* buf  = vmem_alloc_huge(bytes);        // best effort; degrades to normal pages
// ... use buf ...
vmem_free(buf);                             // same free path as vmem_alloc

void* nodeBuf = vmem_alloc_numa_huge(bytes, numa_domain_id(0));   // + node-bound
vmem_free_numa(nodeBuf);
```

- **`vmem_huge_pages_available()`** - true if this process can actually get 2 MB
  pages right now (attempts to enable them on first call; memoized after that).
- **`vmem_huge_page_bytes()`** - `2097152` where available, else `0`.
- **`vmem_alloc_huge(size)`** / **`vmem_alloc_numa_huge(size, node)`** - reserve+commit,
  preferring huge pages. **Degrade-silently, query-explicitly**: these never fail an
  allocation just because huge pages are unavailable - they hand back normal-page
  memory instead, and a caller that cares (a benchmark labelling its run, a kernel
  choosing a strategy) calls `vmem_huge_pages_available()` first rather than getting
  an out-param or a global "was that huge" flag. Freed by the existing `vmem_free` /
  `vmem_free_numa` - there is no separate huge-page free entry point.

### Per-platform admin steps

| Platform | Mechanism | To enable |
|---|---|---|
| Windows | `VirtualAlloc(..., MEM_LARGE_PAGES, ...)` | Needs the `SeLockMemoryPrivilege` account right: `secpol.msc` -> Local Policies -> User Rights Assignment -> "Lock pages in memory" -> add the account -> **log out and back in** (a fresh process is required for the privilege to take effect). Without it, `vmem_alloc_huge` silently returns normal pages. |
| Linux | transparent huge pages (`madvise(MADV_HUGEPAGE)`) | Check `/sys/kernel/mm/transparent_hugepage/enabled`; `madvise` or `always` mode both work (system-wide `always` needs **no code change at all** - every mapping is a THP candidate). `never` mode disables the lever entirely; `vmem_huge_pages_available()` reports it honestly either way. No privilege needed - THP is best-effort by design, unlike `MAP_HUGETLB` which needs preallocated `vm.nr_hugepages` and hard-fails without them. |
| macOS | none | Apple Silicon has no usable huge-page lever (`VM_FLAGS_SUPERPAGE_SIZE_2MB` is x86-64-only and effectively dead there). `vmem_huge_pages_available()` always returns `false` - the honest no-op, same posture as the NUMA/CPU-affinity no-op tier on this platform. |

Windows large-page allocation can still fail even with the privilege held, on
a fragmented system that lacks a physically contiguous 2 MB block - the
library retries once without `MEM_LARGE_PAGES` and hands back normal pages
rather than failing the call; this is expected occasionally on a long-uptime
box, not a bug report.

## Memory-mapped files (`MappedFile`)

`import "filesystem.cb";` gives `MappedFile`, an owning, whole-file memory mapping - the
missing INPUT side of the HPC story: G1-G4 built the noalias/vectorize machinery, and a
mapping is its natural producer. Reading a large input with `File.readBytes` copies every
byte into a heap buffer first; `MappedFile` hands the page-cache-backed pages straight to
`vectorize` / `parallel_for_n` as a first-class noalias `span<T>`, with **zero copy**.

```c
import "filesystem.cb";

MappedFile* m = new MappedFile;
if (m->openRead("dataset.bin")) {
    span<double> data = m->asSpan<double>();            // zero-copy noalias view
    double[] v = data.data();
    double sum = 0.0;
    vectorize(reassoc) for (i64 i = 0; i < data.length(); i = i + 1) { sum = sum + v[i]; }
}
delete m;   // destructor unmaps
```

- **`openRead(path)`** maps the whole file read-only. **`openWrite(path, len)`** maps
  `len` bytes read-write, fixing the file at exactly that size **before** the mapping is
  created - the mapping cannot grow the file afterward, so `len` is not optional. Both
  release any mapping already held first (re-opening a `MappedFile` is safe), and both are
  whole-file only in v1: there is no windowed `map(offset, len)` and no copy-on-write mode.
- **`length()`** / **`data()`** - the mapping's byte length and raw `i8*` base.
- **`asSpan<T>()`** is the payoff: a noalias `span<T>` over the whole mapping, handed
  straight to `vectorize` / `parallel_for_n` with zero copy (`m->asSpan<double>()` on a
  `MappedFile*`, `f.asSpan<double>()` on a value).
- **`flush()`** writes a writable mapping's dirty pages back to the file
  (`FlushViewOfFile` / `msync`). **`close()`** unmaps and is idempotent; the destructor
  calls it, so an RAII-scoped `MappedFile` needs no explicit cleanup (same shape as
  `VMemRegion` in `core/vmem.cb`).

### Traps

- **An empty file maps cleanly, not as an error.** Windows cannot create a file mapping
  over a zero-length file at all (`CreateFileMapping` fails outright), so a 0-byte file is
  special-cased to `length() == 0`, `data() == nullptr`, `openRead` returning `true` - on
  every platform, for consistency. `asSpan<T>()` on an empty mapping returns an
  empty span rather than dereferencing a null base.
- **A write mapping cannot grow the file.** `openWrite`'s `len` sets the FINAL size before
  anything is mapped (`SetEndOfFile` / `ftruncate`, run first). Appending through a mapping
  is not a thing - write the desired final size up front.
- **Truncating a file under a live mapping is a hard fault, not an error code.** `SIGBUS`
  on POSIX, `EXCEPTION_IN_PAGE_ERROR` on Windows, on the next access to the truncated
  pages. This is the irreducible price of mapping a file that something else can still
  resize; there is no guard against it because there is no way to win that race. Don't
  truncate (or let another process/thread truncate) a file you have mapped.
- **Huge pages do not back mapped files.** Windows large pages only back private committed
  memory, not real file mappings (`SEC_LARGE_PAGES` is pagefile-backed sections only); Linux
  THP for read-only file-backed page cache is narrow and kernel-config-dependent, not
  something a library can rely on. So do not expect the huge-page support to speed up a
  mapping. The actionable lever for a mapping's performance is prefetch/advice
  (`madvise(MADV_WILLNEED)` / `PrefetchVirtualMemory`), not page size - not yet implemented.

### Copy vs. zero-copy: what `performance/hpc/mmap_scan_bench.cb` actually shows

The benchmark sums a 256 MB file of doubles two ways - `File.readBytes` into a heap buffer
vs. `MappedFile.asSpan<double>()` fed straight to a `vectorize(reassoc)` reduction - and
reports the honest result rather than an assumed win: on a **warm page-cache, single-process
run** (the file was just written by the same process, so the pages are already resident),
the **copy variant was faster**, not the mapped one - the first-touch soft page faults on a
fresh mapping's pages cost more, in this shape of run, than one bulk `read()` syscall plus a
memcpy-speed scan. Zero-copy is not automatically a win; it removes ONE cost (the user-space
copy) while potentially adding another (page-fault-driven, one-page-at-a-time population of
the mapping) that a single sequential `read()` does not pay. Where `MappedFile` is expected
to win is a **cold, multi-GB, page-cache-resident-across-many-reads** dataset (avoiding the
copy on every subsequent pass) or a **randomly-accessed** dataset (no need to read the parts
never touched) - re-run the benchmark for your actual access pattern before assuming either
direction.

## Numeric kernel libraries (`core/hpc`)

The features above are the *language surface*; `core/hpc/` ships a small set of ready-made
numeric kernels built on top of them. They are normal core libraries - import with the `hpc/`
prefix - and they exist to be both useful and a worked example of the conventions on this page:
distinct `double[]` array-views are `noalias`, the pure element-wise maps carry `vectorize`, and
the floating-point reductions are deliberately left scalar (a reduction reorders adds, so the
`vectorize` contract correctly refuses it).

| Import | Exposes | Contents |
|--------|---------|----------|
| `import "hpc/vecmath.cb";`  | `namespace Vec`     | BLAS-1: `axpy`, `scal`, `copy`, `fill`, `dot`, `asum`, `nrm2`, `iamax` |
| `import "hpc/densemat.cb";` | `namespace Mat`     | BLAS-2/3: `gemv`, `gemm` (cache-blocked, optionally pool-parallel), `gemm_ld` (sub-block / leading-dimension variant), `transpose` over row-major `double[]` |
| `import "hpc/stencil.cb";`  | `namespace Stencil` | `jacobi1d`, `laplace1d`, `jacobi2d` (5-point), `boxblur2d` (3x3) |
| `import "hpc/scan.cb";`     | `namespace Scan` + `Welford` | `prefix_sum_inclusive/exclusive`, `sum`/`minval`/`maxval`, streaming mean+variance |
| `import "hpc/fft.cb";`      | `namespace FFT`     | iterative radix-2 Cooley-Tukey `fft`/`ifft` + `dft_naive` reference |
| `import "hpc/sparse.cb";`   | `struct CsrMatrix`  | compressed sparse row matrix + `spmv` |
| `import "hpc/factor.cb";`   | `namespace Factor`  | dense direct solvers: Cholesky / LU (Doolittle) / LU with partial pivoting (optionally pool-parallel) + forward/back substitution |
| `import "hpc/solvers.cb";`  | `namespace Solver`  | iterative sparse solvers: Conjugate Gradient + Jacobi over `CsrMatrix` |

`factor` and `solvers` show the intended layering: `Solver.cg_solve` reimplements no kernel - every
matrix-vector product is `CsrMatrix.spmv` and every vector op is a `Vec.*` call, exactly how a real
Krylov library sits on top of BLAS plus a sparse format.

```c
import "hpc/vecmath.cb";
import "hpc/sparse.cb";
import "hpc/solvers.cb";

CsrMatrix a;  /* ...fill CSR... */
double[] b = new double[n];
double[] x = new double[n];
Vec.fill(x, 0.0, n);                                 // initial guess
double res = 0.0;
int iters = Solver.cg_solve(&a, b, x, n, 1e-9, 1000, &res);   // x now solves A x = b
```

The convention everywhere is the BLAS one: lengths travel alongside the view (a `double[]` array-view
is thin and carries no length), buffers are allocated up front with `new T[n]` and freed with
`delete[_]`, and the hot paths do not allocate.

Correctness for all eight is checked by [`Test/test_hpc_kernels.cb`](../Test/test_hpc_kernels.cb)
(run by `test.bat`); throughput numbers and a write-up live in
[`performance/hpc/README.md`](../performance/hpc/README.md).

Two end-to-end worked examples drive these libraries the way an application would:
[`example/hpc/poisson_cg.cb`](../example/hpc/poisson_cg.cb) assembles the 2D Poisson 5-point
operator as a `CsrMatrix` and solves it with `Solver.cg_solve` against a manufactured solution,
and [`example/hpc/lu_bench.cb`](../example/hpc/lu_bench.cb) benchmarks the three `Factor` LU
variants (unpivoted serial, partial-pivot serial, partial-pivot pool-parallel) on a diagonally
dominant dense system with a normwise backward-error check via `Mat.gemv`.

`Mat.gemm(alpha, a, b, beta, c, m, k, n, pool = nullptr, workers = 0)` is cache-blocked: the
output C is tiled 64x64 (`GEMM_MB`/`GEMM_NB`, sized so a tile stays ~L1-resident on a 48KB L1d),
and for each tile the A and B sub-blocks of each 256-deep k slab (`GEMM_KB`) are packed into
small contiguous scratch buffers - ~288KB per k step, comfortably inside a 1MB L2 - before a
tight unrolled kernel runs over the packed tiles. Each element accumulates its k terms in
ascending order, so the result is deterministic. Passing a resident `ThreadPool*` partitions
the OUTPUT tiles across workers once `m*k*n` clears `Mat.GEMM_PAR_MIN_WORK` (the dispatch-cost
cutoff above); each output element is computed by exactly one worker in the serial per-tile
operation order, so the pooled product is bit-identical to the serial one. Measured at `-O2`
on a 10-core machine: ~21 GFLOP/s serial and ~55 GFLOP/s pooled (8 workers) at n = 1024-2048,
flat across sizes - the naive i-k-j loop it replaced fell off a cliff once B left cache.
`Mat.gemm_ld` is the same kernel with explicit leading dimensions (row strides), for operating
on sub-blocks of a larger matrix - e.g. a view re-blessed at `(double[])&lu[i0 * n + j0]` with
`ld = n`; `gemm` forwards to it with the leading dimensions equal to the row lengths.

`Factor.lu_factor_piv(a, lu, piv, n, pool = nullptr, workers = 0)` is the general-matrix entry
point: partial pivoting removes the diagonal-dominance requirement at no measured cost. At
n >= `Factor.LU_BLOCK_MIN_N` (384) it switches to a blocked right-looking algorithm: panel
factor over `LU_NB` (128) columns with the classic pivoted rank-1 logic, a unit-L triangular
solve for the panel's block row of U, then ONE `Mat.gemm_ld` trailing update (alpha = -1,
beta = 1) per panel - which keeps the dominant update compute-bound instead of streaming the
whole trailing matrix per column. The pivot sequence and the packed L/U layout match the
unblocked sweep exactly (`lu_solve_piv` works on either), and a `ThreadPool*` passed through
parallelizes the trailing gemm over output tiles, so the pooled factorization stays
bit-identical to the serial blocked one - no reassociated reductions, so no numeric caveat.
Below the threshold the small-n unblocked sweep runs as before (its pool path fans the rank-1
update out per row over `LU_PAR_MIN_WORK`). Measured at `-O2` on the same machine, the old
serial sweep decayed from ~10.6 GFLOP/s at n = 1024 to ~3.3 at n = 3072 once the matrix left
L3; the blocked path holds ~16.3 / 16.6 / 18.4 GFLOP/s at n = 1024 / 2048 / 3072 serial and
~24 / 35 / 40 GFLOP/s pooled (8 workers) - roughly 5.6x serial and 3.4x pooled over the old
numbers at n = 3072.
