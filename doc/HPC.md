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
- [`span<T>` (noalias) and `view<T>` (may-alias)](#spant-noalias-and-viewt-may-alias)
- [Across-core data parallelism (`parallel_for_n` / `parallel_reduce`)](#across-core-data-parallelism-parallel_for_n--parallel_reduce)
- [Numeric kernel libraries (`core/hpc`)](#numeric-kernel-libraries-corehpc)

**Related:** [Threading & Memory Management](threading.md) (threads, affinity, thread pool, allocators, lock-set analysis) | [Language & Core Library Features](LANGUAGE.md)

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
- **Runtime alias checks are accepted.** When the compiler cannot prove the loop's
  pointers are disjoint, the optimizer emits a runtime overlap check guarding the
  vector path (with a scalar fallback). That still satisfies the contract.
- A loop fails the contract when it is not vectorizable: no countable trip count
  (e.g. `vectorize while (p != nullptr)` over a linked list), a loop-carried
  dependence (`a[k] = a[k-1] + 1`), or a non-inlinable call in the body.
- Integer reductions (`sum += a[k]`) vectorize. Floating-point reductions are not
  supported yet (they would require reassociating the additions, which changes the
  numeric result) and are reported as a failure.
- Only counted `for` and `while` are allowed; `do`/`while` and `foreach` are rejected
  up front (`foreach` lowers to `count()`/`get()` calls that cannot vectorize).

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
- **Lane access**: `v[i]` reads lane `i`.
- It is a **primitive value**, not a struct or array - no ownership, destructor, or `move`
  interaction. (Comparisons, lane writes, shuffles, and load/store against `T[]` are not yet
  supported.)

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
may-alias (two sub-views are allowed to overlap). `span<T>` deliberately has **no** general
`slice`/`subspan`: an arbitrary offset span (e.g. `s.subspan(0,10)` and `s.subspan(5,10)`) could
overlap while looking distinct, reopening the aliasing the noalias contract closes.

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

`workers <= 0` selects a default (`hardware_concurrency()`, or the pool's worker count for the pool
variants); `workers` is clamped to `n`. CFlat lambdas do not capture, so the worker reads its buffers
and scalars through a borrowed `void* ctx` (a caller-defined context struct) plus its `[lo, hi)` range.

### `parallel_for_n` - parallel map

```c
struct AxpyCtx { double[] y; double[] x; double alpha; }

AxpyCtx c;  c.y = y;  c.x = x;  c.alpha = 2.5;
parallel_for_n(n, 0, (i64 lo, i64 hi, void* raw) => {     // 0 -> hardware_concurrency()
    AxpyCtx* k = (AxpyCtx*)raw;
    double[] y = k->y;  double[] x = k->x;  double a = k->alpha;
    for (i64 i = lo; i < hi; i = i + 1) { y[i] = y[i] + a * x[i]; }
}, &c);
```

The body is called once per worker (not once per element), so there is no per-element call overhead.
A `void`-returning body lambda can be written inline as shown.

### `parallel_reduce<T>` - parallel reduction

Each worker folds its chunk into a partial `T`; the helper merges the partials with `combine` (seeded
by `identity`) after all workers finish. Reductions are exactly the kernels that *cannot* use
`vectorize` for the cross-element accumulation (FP reassociation changes the result), so threads are
their only coarse parallelism - while the per-chunk `partial` loop can still be a `vectorize`/`simd`
reduction, stacking SIMD under threads.

```c
struct DotCtx { double[] a; double[] b; }

// A value-returning lambda passed inline as an argument does not pick up its
// return type, so partial/combine are named functions (also the clearer pattern).
double dotPartial(i64 lo, i64 hi, void* raw) {
    DotCtx* k = (DotCtx*)raw;
    double[] a = k->a;  double[] b = k->b;
    double s = 0.0;
    for (i64 i = lo; i < hi; i = i + 1) { s = s + a[i] * b[i]; }
    return s;
}
double addD(double x, double y) { return x + y; }

DotCtx c;  c.a = a;  c.b = b;
double dot = parallel_reduce<double>(n, 4, 0.0, dotPartial, addD, &c);
```

`combine` must be associative. Because both SIMD lane-reduction *and* the per-worker split reassociate
floating-point adds, the result depends on `workers` and is **not** bit-identical to a serial fold -
standard and expected for parallel/SIMD reductions.

For an **integer** reduction (a count, a histogram bin, a checksum) `T` is `i64` and `identity` is the
zero literal cast to the element type - `(i64)0`, not `0` (an unannotated `0` is `int`, which will not
match the `i64` reduction):

```c
i64 addI(i64 x, i64 y) { return x + y; }                 // combine
i64 count = parallel_reduce<i64>(n, 0, (i64)0, countPartial, addI, &c);
```

### Pinning workers to cores (`pin`)

The raw-thread `parallel_for_n` and `parallel_reduce<T>` take a trailing `bool pin = false`. Set it to
`true` to pin worker `w` to core `w % hardware_concurrency()` (via `SetThreadAffinityMask`), which
removes the OS scheduling jitter that otherwise caps speedup on compute-bound kernels - the single most
important knob for steady throughput (see [threading.md](threading.md)):

```c
i64 count = parallel_reduce<i64>(n, 0, (i64)0, countPartial, addI, &c, /*pin=*/true);
```

Leave it `false` when the region oversubscribes the machine or shares cores with other work. The
`*_pool` variants do not expose `pin`: the `ThreadPool` owns its worker threads, so pin them once at
pool construction instead.

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

struct PiCtx { i64 drawsPerSample; }   // 2 draws (x, y) per sample

i64 piPartial(i64 lo, i64 hi, void* raw) {
    PiCtx* k = (PiCtx*)raw;
    Random rng = default;
    rng.seed(0x1234567);               // same base seed on every worker...
    rng.jump(lo * k->drawsPerSample);  // ...then jump to this chunk's window (no overlap)
    i64 inside = 0;
    for (i64 i = lo; i < hi; i = i + 1) {
        double x = rng.nextDouble();
        double y = rng.nextDouble();
        if (x * x + y * y <= 1.0) { inside = inside + 1; }
    }
    return inside;
}
// pi ~= 4 * parallel_reduce<i64>(n, 0, (i64)0, piPartial, addI, &ctx, true) / n
```

### Pool variants

The pool variants take a `ThreadPool*` first argument and otherwise match. Amortize the worker threads
across a hot loop:

```c
ThreadPool pool;  pool.init(4);
for (int step = 0; step < 1000; step++) {
    double r = parallel_reduce_pool<double>(&pool, n, 0, 0.0, dotPartial, addD, &c);
    // ... use r ...
}
pool.shutdown();
```

> The pool variants are named `*_pool` rather than overloading the raw names: two generic functions of
> the same name monomorphize to the same mangled symbol and would collide, so both pool entry points
> carry the suffix for a uniform API.

---

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
| `import "hpc/densemat.cb";` | `namespace Mat`     | BLAS-2/3: `gemv`, `gemm` (i-k-j), `transpose` over row-major `double[]` |
| `import "hpc/stencil.cb";`  | `namespace Stencil` | `jacobi1d`, `laplace1d`, `jacobi2d` (5-point), `boxblur2d` (3x3) |
| `import "hpc/scan.cb";`     | `namespace Scan` + `Welford` | `prefix_sum_inclusive/exclusive`, `sum`/`minval`/`maxval`, streaming mean+variance |
| `import "hpc/fft.cb";`      | `namespace FFT`     | iterative radix-2 Cooley-Tukey `fft`/`ifft` + `dft_naive` reference |
| `import "hpc/sparse.cb";`   | `struct CsrMatrix`  | compressed sparse row matrix + `spmv` |
| `import "hpc/factor.cb";`   | `namespace Factor`  | dense direct solvers: Cholesky / LU (Doolittle) + forward/back substitution |
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
