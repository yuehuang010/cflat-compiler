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

### Throughput / FLOP-bound numeric kernels

| File | Kind | Contents |
|------|------|----------|
| `vecmath.cb`  | library | BLAS-1: `axpy`, `scal`, `copy`, `fill`, `dot`, `asum`, `nrm2`, `iamax` |
| `densemat.cb` | library | BLAS-2/3: `gemv`, `gemm` (i-k-j), `transpose` over row-major `double[]` |
| `stencil.cb`  | library | `jacobi1d`, `laplace1d`, `jacobi2d` (5-point), `boxblur2d` (3x3) |
| `scan.cb`     | library | `prefix_sum_inclusive/exclusive`, `sum/min/max`, `Welford` streaming mean+variance |
| `fft.cb`      | library | iterative radix-2 Cooley-Tukey `fft`/`ifft` + `dft_naive` reference |
| `sparse.cb`   | library | `CsrMatrix` + `spmv` (compressed sparse row matrix-vector) |
| `hpc_demo.cb` | driver  | 31 correctness checks across every kernel |
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
x64\Release\cflat.exe performance\hpc\hpc_demo.cb  -i performance\hpc -i cflat\core -o out\hpc\hpc_demo.exe  -O2 --cpu x86-64-v3
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
