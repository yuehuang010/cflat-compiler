# Single-threaded HPC library suite

A customer-style evaluation of CFlat for high-performance numerical code. The goal was
to write a small but representative BLAS/PDE/FFT/sparse library *as a user would* - using
only the public language surface and core libraries - and see how close the generated code
gets to hand-tuned C, where the language helps, and where it gets in the way.

The suite covers two regimes of HPC: the **throughput / FLOP-bound** numeric kernels
(BLAS, stencils, FFT, sparse) whose story is auto-vectorization, and one
**latency / cache-hierarchy bound** structure (`bptree.cb`, a B+ tree) whose story is
hiding cache misses - shipped with a line-for-line C++ reference so it doubles as a direct
codegen-parity check.

## What's here

The numeric kernel libraries now ship in the standard library under
[`cflat/core/hpc/`](../../cflat/core/hpc) and are imported with the `hpc/` prefix
(`import "hpc/vecmath.cb";`). The drivers and benchmarks below stay here in `performance/hpc/`.

### Throughput / FLOP-bound numeric kernels

| File | Kind | Contents |
|------|------|----------|
| `core/hpc/vecmath.cb`  | library | BLAS-1: `axpy`, `scal`, `copy`, `fill`, `dot`, `asum`, `nrm2`, `iamax` |
| `core/hpc/densemat.cb` | library | BLAS-2/3: `gemv`, `gemm` (i-k-j), `transpose` over row-major `double[]` |
| `core/hpc/stencil.cb`  | library | `jacobi1d`, `laplace1d`, `jacobi2d` (5-point), `boxblur2d` (3x3) |
| `core/hpc/scan.cb`     | library | `prefix_sum_inclusive/exclusive`, `sum/min/max`, `Welford` streaming mean+variance |
| `core/hpc/fft.cb`      | library | iterative radix-2 Cooley-Tukey `fft`/`ifft` + `dft_naive` reference |
| `core/hpc/sparse.cb`   | library | `CsrMatrix` + `spmv` (compressed sparse row matrix-vector) |
| `core/hpc/factor.cb`   | library | `Factor.*` dense direct solvers: Cholesky / LU (Doolittle) + forward/back substitution |
| `core/hpc/solvers.cb`  | library | `Solver.*` iterative sparse solvers: Conjugate Gradient + Jacobi over `CsrMatrix` |
| `core/hpc/parallel.cb` | library | `parallel_for_n` / `parallel_reduce<T>` (+ `_pool` variants) - across-core data parallelism |
| `Test/test_hpc_kernels.cb` | test    | 38 correctness checks across every kernel (run by `test.bat` and as build_hpc's correctness step) |
| `hpc_bench.cb`| driver  | throughput benchmark (GFLOP/s, GB/s) |

### Latency / cache-hierarchy bound

| File | Kind | Contents |
|------|------|----------|
| `bptree.cb`  | driver + ref | cache-line-blocked B+ tree (ordered `i64 -> i64` map); self-contained correctness + throughput benchmark |
| `bptree.cpp` | C++ reference | line-for-line MSVC `/O2` port of the same benchmark for codegen comparison |

## Build & run

```bat
performance\hpc\build_hpc.bat               REM Release, AVX2 (x86-64-v3)
performance\hpc\build_hpc.bat Release znver4 REM AVX-512
```

Or manually:

```bat
x64\Release\cflat.exe Test\test_hpc_kernels.cb     -i cflat\core -o out\hpc\test_hpc_kernels.exe -O2 --cpu x86-64-v3
x64\Release\cflat.exe performance\hpc\hpc_bench.cb -i performance\hpc -i cflat\core -o out\hpc\hpc_bench.exe -O2 --cpu x86-64-v3
```

## Design conventions used

- **`double[]` array-views everywhere.** Distinct views are `noalias`, so the pure-map
  kernels (`axpy`, `scal`, `copy`, `fill`, the `gemm` inner loop, the stencil interiors)
  vectorize at `-O2` with **no runtime alias check**. Verified in the IR: `axpy` lowers to
  `define void @...axpy...(double %alpha, ptr noalias nocapture readonly %x, ptr noalias
  nocapture %y, i32 %n)`.
- **`vectorize` on the pure maps only.** The keyword is a *checked contract* - it
  hard-errors if the loop does not vectorize - so it is a perfect regression guard. It is
  deliberately **not** placed on floating-point reductions (`dot`, `asum`, `nrm2`, the
  `gemv`/`spmv` inner loops): an FP reduction reorders additions and changes the result, so
  the contract (correctly) refuses it. Those stay scalar.
- **Length carried alongside the view** (the BLAS/C convention); a view is thin and carries
  no length.
- **Single-threaded, allocation-free hot paths.** Buffers are allocated up front with
  `new T[n]` and freed with `delete[_]`.

## Results (Ryzen AI 9 365, Release, single thread)

GEMM scales cleanly with the vector ISA, which is the headline that auto-vectorization is
real:

| Kernel | `--cpu x86-64` (SSE2) | `--cpu x86-64-v3` (AVX2) | `--cpu znver4` (AVX-512) |
|--------|----------------------:|-------------------------:|-------------------------:|
| gemm 256^3 | 15.3 GFLOP/s | 29.7 GFLOP/s | **60.0 GFLOP/s** |
| jacobi2d 2048^2 | 7.1 GFLOP/s | 8.6 GFLOP/s | 9.0 GFLOP/s |
| axpy (bandwidth) | 57 GB/s | 55 GB/s | 58 GB/s |
| dot | 3.3 GFLOP/s | 2.8 GFLOP/s | 3.3 GFLOP/s |
| fft 2^20 | - | 4.1 GFLOP/s | - |
| spmv tridiag 2^20 | - | 3.2 GFLOP/s | - |

`axpy`/`dot` are memory-bound (≈60 GB/s, flat across ISA - exactly as expected for a
streaming kernel out of cache). `gemm` is compute-bound and roughly doubles per ISA level,
confirming the inner loop is genuinely SIMD.

## External BLAS: OpenBLAS `cblas_dgemm` vs `Mat.gemm`

`Mat.gemm` (above) is a self-contained, dependency-free kernel. The other end of the spectrum
is a hand-tuned vendor BLAS. [`example/vcpkg/blas_gemm.cb`](../../example/vcpkg/blas_gemm.cb) binds
**OpenBLAS** straight from its C header with CFlat's `import package-vcpkg` machinery and
benchmarks `cblas_dgemm` against `Mat.gemm` at three sizes - a worked example of "call the vendor
library when you want peak, keep the core kernel when you want a self-contained build."

### Getting OpenBLAS (via the example-local vcpkg manifest)

The demo sources OpenBLAS through vcpkg, the same way `example/vcpkg/sqlite_demo.cb`,
`sdl3_demo.cb`, and `zlib_demo.cb` source their packages: `openblas` is a dependency in
[`example/vcpkg/vcpkg.json`](../../example/vcpkg/vcpkg.json) (the repo's root `vcpkg.json` stays
untouched). No download, no `%BLAS_SDK%` env var, no CLI paths - `cflat.exe` invokes
`vcpkg install` itself against that manifest on first compile and resolves the include dir, link
lib, and runtime DLL automatically. `example.bat` builds `blas_gemm.cb` unconditionally as part of
the `example/vcpkg/` sweep (there is no SDK-discovery gate to skip).

The first compile triggers a **source build** of OpenBLAS (vcpkg has no prebuilt binary cache for
it here) - expect it to take 10-30 minutes once; subsequent compiles reuse the installed package.
A manifest change to the `openblas` dependency (e.g. adding/removing a feature) triggers a full
rebuild of the port on the next compile - also 10-30 minutes.

- Version installed via vcpkg: **OpenBLAS 0.3.33** (same upstream version as the previous external
  SDK).
- The manifest pins `{ "name": "openblas", "features": ["threads"] }` - vcpkg's `openblas` port
  gates multithreading behind an opt-in `threads` feature (`USE_THREAD`, off by default); without
  it the built library is single-threaded regardless of host core count. **The `threads` feature is
  required to get anywhere near peak** - see Results below for the measured gap.

### The working bind

The source names *what* to bind; vcpkg resolves everything else from the manifest:

```c
import package-vcpkg "openblas/cblas.h" from "openblas";   // cblas_dgemm + CBLAS_ORDER/CBLAS_TRANSPOSE enums
...
cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, m, n, k,
            1.0, &a[0], k, &b[0], n, 0.0, &c[0], n);
```

```bat
x64\Release\cflat.exe example\vcpkg\blas_gemm.cb -o out\hpc\blas_gemm.exe -O2 --cpu x86-64-v3
out\hpc\blas_gemm.exe            REM square 512 (default); pass an N for another size
```

`cblas_dgemm`, `CblasRowMajor`, and `CblasNoTrans` all come straight from `cblas.h` (installed at
`example/vcpkg/vcpkg_installed/x64-windows/include/openblas/cblas.h`) - no hand-written prototype,
no manual enum values. The `core/hpc` import (`hpc/densemat.cb`) resolves against the deployed
`core/` tree next to `cflat.exe`, so no `-i` is needed. **Runtime DLL:** after the link the
compiler auto-copies `openblas.dll` next to the exe, so it launches with nothing extra on `PATH`.

### Results (Ryzen AI 9 365, Release, `--cpu x86-64-v3`, best of 3 per process)

The table below was originally measured against the **prebuilt OpenBLAS 0.3.33 Windows release**
(`OpenBLAS-0.3.33-x64.zip`), which ships built with multithreading enabled (20 logical cores on
this host); `Mat.gemm (pool)` is the core kernel fanned out over 8 pinned workers; `Mat.gemm
(serial)` is single-threaded. Each size was run in a fresh process.

| N (N x N x N) | OpenBLAS `cblas_dgemm` | `Mat.gemm` (serial) | `Mat.gemm` (pool, 8w) |
|---------------|-----------------------:|--------------------:|----------------------:|
| 256  | 0.266 ms / **126.3** GFLOP/s | 1.098 ms / 30.6 GFLOP/s | 0.565 ms / 59.4 GFLOP/s |
| 512  | 1.094 ms / **245.3** GFLOP/s | 9.642 ms / 27.8 GFLOP/s | 2.631 ms / 102.0 GFLOP/s |
| 1024 | 6.992 ms / **307.2** GFLOP/s | 67.49 ms / 31.8 GFLOP/s | 21.65 ms / 99.2 GFLOP/s |

The demo now sources OpenBLAS from vcpkg (`example/vcpkg/vcpkg.json`) instead of that prebuilt
package. Two vcpkg port features affect performance; both were measured honestly, and a third
(`dynamic-arch`, to try to close the remaining gap to the prebuilt release) was attempted and
turned out to be blocked on this triplet - see below.

| Config | `openblas` manifest entry | What it changes |
|---|---|---|
| prebuilt release (orig.) | n/a - external `OpenBLAS-0.3.33-x64.zip` | Vendor build: multithreaded + `DYNAMIC_ARCH` (runtime CPUID-dispatched micro-kernels) |
| vcpkg default | `"openblas"` | Single-threaded, one generic kernel |
| vcpkg + `threads` (current manifest) | `{ "name": "openblas", "features": ["threads"] }` | Multithreaded (`USE_THREAD`), still one generic kernel |
| vcpkg + `threads` + `dynamic-arch` | *(attempted, not usable - see below)* | Would add the CPUID-dispatched micro-kernels |

**vcpkg default (no features) - single-threaded:**

| N (N x N x N) | OpenBLAS `cblas_dgemm` (vcpkg default) | `Mat.gemm` (serial) | `Mat.gemm` (pool, 8w) |
|---------------|-----------------------------------------:|--------------------:|----------------------:|
| 1024 | 305.9 ms / **7.0** GFLOP/s | 83.0 ms / 25.9 GFLOP/s | 29.0 ms / 74.1 GFLOP/s |

`openblas_get_num_procs()` / `openblas_get_num_threads()` report **1 proc, 1 thread** here,
regardless of the host's 20 logical cores - the library is single-threaded because `USE_THREAD`
was off. At N=1024 that is ~44x slower than the prebuilt multithreaded release, and even slower
than single-threaded `Mat.gemm` - the opposite of the "vendor BLAS wins on peak throughput"
takeaway the demo makes with the prebuilt package.

**vcpkg + `threads` (current manifest) - multithreaded, generic kernel:**

| N (N x N x N) | OpenBLAS `cblas_dgemm` (vcpkg, `threads`) | `Mat.gemm` (serial) | `Mat.gemm` (pool, 8w) |
|---------------|---------------------------------------------:|--------------------:|----------------------:|
| 256  | 1.029 ms / **32.6** GFLOP/s | 1.407 ms / 23.9 GFLOP/s | 0.779 ms / 43.1 GFLOP/s |
| 512  | 6.852 ms / **39.2** GFLOP/s | 12.439 ms / 21.6 GFLOP/s | 3.702 ms / 72.5 GFLOP/s |
| 1024 | 65.633 ms / **32.7** GFLOP/s | 99.807 ms / 21.5 GFLOP/s | 30.314 ms / 70.8 GFLOP/s |

With `threads` enabled, `openblas_get_num_procs()` / `openblas_get_num_threads()` correctly report
**20 procs, 20 threads** - the port now sees and uses the real core count. Throughput is much
better than the single-threaded default (roughly 4-6x) but still well short of the prebuilt
release's 126-307 GFLOP/s, and `Mat.gemm` (pool, 8w) *beats* `cblas_dgemm` at every size measured
(run-to-run variance on this heterogeneous host is real - see the cache-topology note earlier in
this doc - but the gap to `Mat.gemm` pool is consistent across repeated runs, not noise).

**`dynamic-arch` attempted, not usable on this triplet:** the vcpkg port declares
`openblas[dynamic-arch]` (`"Support for multiple targets in a single library"`, i.e. `DYNAMIC_ARCH`)
as `"only supported on '!windows | mingw'"` - MSVC's `x64-windows` triplet is explicitly excluded.
Adding `{ "name": "openblas", "features": ["dynamic-arch", "threads"] }` fails the install outright:

```
openblas[dynamic-arch] is only supported on '!windows | mingw', which does not match x64-windows.
```

vcpkg does offer `--allow-unsupported` to force it anyway, but the port's own message warns of
"known build failures, or runtime problems" doing so - not attempted here without a deliberate,
separately-approved follow-up (and it would need `--allow-unsupported` threaded through
`VcpkgResolver`'s `vcpkg install` invocation, a compiler-side change, not just a manifest edit).
vcpkg rejects the feature at the supports-expression check before any compilation starts, so no
`dynamic-arch` build ever ran here and there is no DLL-size comparison to report; the current
`openblas.dll` (`threads` only, no `dynamic-arch`) is **2,712,064 bytes** (~2.6 MB).
The manifest was reverted to `{ "name": "openblas", "features": ["threads"] }` (the last known-good,
verified config) after this was discovered. **The remaining gap to the prebuilt release's
per-microarchitecture tuned kernels is therefore a real, currently-unclosed limitation of sourcing
OpenBLAS through vcpkg's MSVC/x64-windows port**, not something this example's configuration can
fix.

**Takeaway.** With the prebuilt, multithreaded, `DYNAMIC_ARCH`-tuned release, OpenBLAS reaches
~307 GFLOP/s at 1024; `Mat.gemm` reaches ~99 GFLOP/s pooled (~32% of OpenBLAS) and ~32 GFLOP/s
single-threaded (~10%), with zero external dependencies. With the vcpkg-managed build this example
now uses, the `threads` feature is required just to get OpenBLAS off single-core (roughly 7 -> 33
GFLOP/s at 1024), and even then `Mat.gemm` (pool) wins by ~2x, because vcpkg's `x64-windows` port
cannot build the `DYNAMIC_ARCH` tuned kernels the prebuilt release ships - a reminder that "vendor
BLAS beats a self-written kernel" is a claim about a *specific, tuned build* of that vendor library,
not the library in the abstract, and that a package manager's default port build is not always that
build. Either way, the header-import path makes swapping one BLAS binding (or build configuration)
for another a two-line change.

## Latency-bound: the B+ tree (`bptree.cb`)

The numeric kernels above are throughput-bound - the question is "does the inner loop
vectorize." `bptree.cb` exercises the *other* axis of HPC: a **latency-bound, cache-conscious
ordered map** where the question is "can you hand-write a cache-aware structure in CFlat and
have codegen keep pace with C++." It is a read-optimized B+ tree (bulk-loaded from sorted
input; insert/split is intentionally omitted) reached for when you need ordered / range
queries and a pointer-chasing BST would thrash the cache.

The techniques it leans on are the cache-hierarchy half of HPC, expressed in plain CFlat:

- **Wide, cache-line-sized nodes** (`FANOUT=16`) keep the tree 3-4 levels deep over a
  million keys - ~3-4 cache misses per lookup instead of ~20 for a binary tree.
- **Keys-only internal nodes (SoA)** so the upper levels stay tiny and L2/L3-resident; values
  live only in the leaves.
- **Index-based children in one contiguous arena** (`array<BNode>`/`array<BLeaf>` + `i32`
  indices, not 8-byte pointers) - denser, and the prefetcher loves the single block.
- **Branchless separator scan** wrapped in `vectorize while` - it replaces a ~50%-mispredicted
  data-dependent branch with an unconditional count reduction, and the `vectorize` contract
  hard-errors if that loop ever stops vectorizing.
- **Software-pipelined batch lookups** (`lookupBatch`, depth 8) - keeps several independent
  probes in flight and `prefetch()`es each leaf, overlapping each L3/DRAM miss with the
  descents of the probes behind it instead of stalling.
- **One-ahead prefetch on range scans** - leaves are linked, so a range query is a linear
  walk that pulls in `leaf->next` a hop early.

`bptree.cb` is self-contained: it builds over 1M keys, checks correctness against a
sorted-array binary-search baseline, verifies the batch path sums identically to the scalar
path, then times all three plus range queries. `bptree.cpp` is a line-for-line MSVC `/O2`
port (identical structure, identical RNG, identical `sink`) so the two are an apples-to-apples
codegen comparison.

```bat
x64\Release\cflat.exe performance\hpc\bptree.cb -i performance\hpc -i cflat\core -o out\hpc\bptree.exe -O2 --cpu x86-64-v3
out\hpc\bptree.exe

performance\hpc\build_cpp.bat        REM builds out\perf\bptree_cpp.exe (MSVC /O2)
out\perf\bptree_cpp.exe
```

### Results (Ryzen AI 9 365, single thread, best of 7 runs)

Both binaries print the same `sink`, confirming identical work. CFlat is at parity with the
MSVC `/O2` reference; the gaps below are within run-to-run noise on this heterogeneous box
(pin affinity for tighter numbers).

| Phase | CFlat (M ops/s) | C++ MSVC `/O2` (M ops/s) |
|-------|----------------:|-------------------------:|
| B+ tree lookup, scalar | 12.0 | 10.9 |
| B+ tree lookup, pipelined batch | 18.6 | 19.6 |
| sorted array + binary search | 8.5 | 9.8 |
| range query (width 1000) | 2.1 | 2.5 |

The headline is that the **pipelined batch path is ~1.7x the scalar path in both languages** -
the `prefetch()`-driven software pipeline does the same latency-hiding work in CFlat as in
C++, and the B+ tree beats the cache-thrashing sorted-array baseline at point lookups.

> Not built by `build_hpc.bat` (which runs the numeric demo + bench); `bptree.cb` has its own
> `main` and is built/run directly as shown above.

## Evaluation notes - what worked, what bit

**Worked well**

- The **`double[]` array-view -> `noalias` -> clean vectorization** pipeline is the real
  deal. No `restrict`, no `#pragma`, no runtime alias-check versioning; the IR shows
  `noalias` and the vector loop with no fallback.
- **`vectorize` as a checked contract** is genuinely useful for a numerics library: a kernel
  that silently stops vectorizing after an edit becomes a hard compile error instead.
- `--cpu x86-64-v3` / `znver4` unlock AVX2 / AVX-512 and the FLOP/s scale accordingly.
- Structs owning an `int[]`/`double[]` field with `alloc`/`dispose` methods work cleanly
  (`CsrMatrix`, and a `Vec`/`Matrix` container shape) - a natural way to package a library.

**Friction hit while writing this**

1. **`1 << N` and `4 * 1024 * 1024` constant-fold WRONG (silent).** *(FIXED 2026-06-07.)*
   Integer literals were stored at their minimal width (`1` is `i8`), and binary constant
   folding evaluated in that narrow type, so the shift/multiply *overflowed at compile time*:
   `1<<7` -> -128, `1<<8` -> 0, `1<<20` -> garbage, `100<<1` -> -56, `4*1024*1024` -> i16
   overflow. The runtime forms were always correct - only compile-time folding was affected.
   This was the most serious finding, since `1 << 20` is the universal idiom for buffer
   sizes. It is now fixed (C integer promotion applied before folding), so the benchmark
   uses the idiomatic `int n = 1 << 22;` directly with no workaround.
2. **`in` is a reserved word** (from `foreach ... in`) and cannot be used as an identifier,
   even as a plain parameter/local name - it fails with a confusing `no viable alternative`
   parser error rather than "reserved word". Renamed every `in` to `src`.
3. **Namespace sibling calls must be qualified.** *(FIXED 2026-06-07.)* Inside
   `namespace FFT`, calling a sibling such as `bit_reverse(...)` used to fail with
   `Undefined variable bit_reverse` and had to be written `FFT.bit_reverse(...)`.
   Unqualified sibling resolution (walking outward through parent namespaces) now works, so
   `fft.cb` calls `bit_reverse` / `transform` unqualified like any in-namespace code.
4. **No `sqrt`/`sin`/`cos` in `core/math.cb`.** *(FIXED 2026-06-07.)* Originally only
   `abs/min/max/clamp/fma` were present, so a numerics user had to hand-declare
   `extern double sqrt(double);` etc. `core/math.cb` now exports `Math.sqrt/sin/cos`
   (double + float), lowered to the `llvm.sqrt`/`llvm.sin`/`llvm.cos` intrinsics so they
   constant-fold and vectorize rather than being opaque CRT calls. `vecmath.cb` and
   `fft.cb` use `Math.sqrt`/`Math.sin`/`Math.cos` directly - no `extern` needed.
5. **`--cpu native` resolves to `generic`** on this Zen5 box (LLVM doesn't recognize the
   CPUID), so it does **not** widen vectors. An explicit `--cpu x86-64-v3` / `znver4` is
   required to get AVX. Minor, but `native` silently giving baseline SSE2 is a trap.

None of these blocked writing the library. Findings (1), (3) and (4) - the silent
const-fold overflow, the unqualified-namespace-sibling resolution, and the missing
transcendentals - have since been fixed in the compiler / core library; (2) (`in` reserved)
and (5) (`--cpu native` -> `generic`) remain as usability papercuts.
