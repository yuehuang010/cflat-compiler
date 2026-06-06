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

## Table of Contents

- [`vectorize` loops](#vectorize-loops)
- [`simd<T,N>` explicit vector type](#simdtn-explicit-vector-type)

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
