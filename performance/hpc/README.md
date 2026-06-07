# Single-threaded HPC library suite

A customer-style evaluation of CFlat for high-performance numerical code. The goal was
to write a small but representative BLAS/PDE/FFT/sparse library *as a user would* - using
only the public language surface and core libraries - and see how close the generated code
gets to hand-tuned C, where the language helps, and where it gets in the way.

## What's here

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
