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

**Related:** [Threading & Memory Management](threading.md) (affinity, allocators, lock-set analysis) | [Language & Core Library Features](LANGUAGE.md)

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
- **One-way conversion, enforced.** Decay `T[] -> T*` is implicit and always safe (it drops
  the guarantee). The reverse - binding a raw `T*` into a `T[]` - is a **compile error** at
  every binding site: declaration-init (`int[] a = p;`), re-assignment (`a = p;`), struct
  field store (`s.view = p;`), passing to a `T[]` parameter (`f(p)`), and `return p;` from a
  `T[]`-returning function. A view therefore comes *only* from `new T[n]`, another `T[]`, or a
  copy of one - which is what makes "two views alias iff they share a base pointer" provable.
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
>   it too: it is usable as an `int*` but cannot be bound back into a `T[]`, so it cannot
>   forge an offset view.
> - One escape hatch remains by design, the same category as Fortran `EQUIVALENCE`: a
>   `union { int* p; int[] v; }` can type-pun a raw pointer into a view.
