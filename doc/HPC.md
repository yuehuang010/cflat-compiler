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
- Integer reductions (`sum += a[k]`) vectorize. Floating-point reductions also
  vectorize: the optimizer reassociates the additions (and reduces the lanes at the
  end), which reorders the sums and so can change the result in the last few ULPs.
  Mark a loop `vectorize` only when that reassociation is acceptable. For a parallel
  FP reduction where you want to control the accumulation explicitly, use
  `parallel_reduce<T>` instead (see below).
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
  FMA; for a plain accumulate, write `acc + a*b` and let it vectorize to `fmul`+`fadd`.
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

To return several results from a kernel - a computed array plus auxiliary scalars - return one
`T[]` and pass the rest as `T[]`/`T*` out-parameters. Each `T[]` parameter keeps its noalias
contract, so the vectorizer proves the buffers pairwise disjoint without a runtime overlap
check. See [Returning Multiple Results](LANGUAGE.md#returning-multiple-results) for the full
pattern - it is a general language feature, not HPC-specific.

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

function<double(i64, i64)> partial = (i64 lo, i64 hi) => {
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
important knob for steady throughput (see [threading.md](threading.md)):

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
pass any bitmask for a custom set (e.g. `cpu_mask_lowest` on a P-core range to keep a solver off the
E-cores). The plain `init(workers)` overload leaves the workers unpinned. This closes the old
pin-vs-pool gap: a bulk-synchronous solver now gets thread reuse **and** affinity at the same time.

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
and never touches the main thread. See [Per-thread floating-point environment](threading.md#per-thread-floating-point-environment-corethreadcb)
in threading.md for the full constant list, the `fp_enable_traps`/`fp_disable_traps` in-thread helpers,
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

function<i64(i64, i64)> piPartial = (i64 lo, i64 hi) => {
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
