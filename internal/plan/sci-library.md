# Sci Library Plan

Status: **S1, S2 and S3 DONE** (2026-08-29), on branch `plan/sci-library`, not
yet committed. S4 remains deferred by design. The three "Open decisions" below were
taken as recommended - `core/sci/`, lowercase `complex`, distributions extended
into `random.cb` in place - and are no longer open. See "As built" at the bottom
for the deviations from the sketches above; the sketches are left unedited so
the diff between plan and outcome stays visible.
Created: 2026-08-29
Source: gap analysis of `cflat/core/hpc/` + `math.cb` + `random.cb` against
what a numerical/scientific user reaches for. Companion to
`internal/plan/hpc-gaps.md`, which covers the COMPILER/runtime gaps (SIMD,
alignment, parallel scheduling). This plan covers only LIBRARY surface and
requires no grammar, backend, or codegen change.

---

## Motivation

`core/hpc/` is a strong HPC *kernel* stack: BLAS-1 (`vecmath.cb`), BLAS-2/3
(`densemat.cb`), direct dense factorization (`factor.cb`), CSR SpMV
(`sparse.cb`), Krylov solvers (`solvers.cb`), radix-2 FFT (`fft.cb`),
stencils, scan/Welford, and the `parallel_for_n` / `parallel_reduce`
fan-out (`parallel.cb`).

What is missing is the layer a scientist actually calls. There is today no
way to compute a median, draw a Gaussian sample, integrate a function,
advance an ODE, fit a line, find a root, or get an eigenvalue. Nor is there
a good way to GET DATA IN: the kernels assume a `double[]` already in
memory, and the obvious way to fill one (read, parse, repeat) leaves the
CPU idle during every read. Those are the routines that make the kernels
reachable, and every one of them is pure CFlat over existing primitives.

Guiding principle, inherited from `solvers.cb`: **the new modules
orchestrate, they do not reimplement.** Any vector op goes through `Vec.*`,
any matrix-vector product through `CsrMatrix.spmv` or `DenseMat.gemv`, any
factorization through `factor.cb`, any thread fan-out through
`parallel.cb`. A new module that opens its own hand-rolled loop over a
`double[]` that `Vec` already covers is a review defect.

---

## Open decisions (need a ruling before S1)

1. **Namespace and directory.** Two options:
   - (a) `core/sci/*.cb` with a `Sci` namespace, sitting ABOVE `core/hpc/`
     and free to import from it. Keeps "kernels" and "methods" visibly
     separate, matching how the code actually layers.
   - (b) Grow `core/hpc/` and reuse its existing per-module namespaces
     (`Vec`, `Scan`, ...). Fewer concepts, but `hpc/` stops meaning
     "kernel" and starts meaning "everything numerical".

   Recommendation: **(a)**. `stats`, `ode`, `roots` are not HPC kernels and
   do not share the noalias/`vectorize` discipline that `hpc/` documents as
   its reason for existing. `doc/HPC.md` stays about kernels; a new
   `doc/SCI.md` covers the methods layer.

2. **`complex` naming.** Lowercase `complex` (matching `string`) or
   `Complex` (matching `Random`, `Welford`, `CsrMatrix`)? The lowercase
   spelling reads better in math code and matches the one other
   compiler-adjacent value type, but every other library struct in core is
   capitalized. Recommendation: lowercase `complex`, on the grounds that it
   is a numeric scalar type and not a container.

3. **Distributions: extend `random.cb` in place, or a separate
   `sci/dist.cb`?** Recommendation: extend `random.cb`. `Random` is already
   the seeded-state owner; splitting the state from the distributions that
   draw on it would force every call site to pass the generator around.

The rest of this plan is written assuming (a), lowercase `complex`, and
in-place `random.cb`.

---

## Sequencing

S1 is the foundation slice and should land as one coherent change. S2 is
the slice that produces a compelling example program, and carries the
pipelined data ingest that makes the whole library usable on real files.
S3 is the hard linear algebra and can be scheduled independently. S4 is
explicitly deferred and listed only so it is not re-proposed.

| Slice | Modules | Depends on | Rough size |
|-------|---------|------------|------------|
| S1 | `sci/complex.cb`, `sci/stats.cb`, `random.cb` distributions, `sci/special.cb` | nothing | ~900 lines |
| S2 | `sci/integrate.cb`, `sci/ode.cb`, `sci/roots.cb`, `sci/interp.cb`, `sci/dataio.cb` | S1 (`special` for tolerances only) | ~1200 lines |
| S3 | `hpc/qr.cb` (+ lstsq), `hpc/eigen.cb`, `fft.cb` completeness | S1 (`complex` for eigen/FFT) | ~900 lines |
| S4 | `sci/poly.cb`, units, sparse direct | S3 (companion-matrix roots need eigen) | deferred |

`sci/dataio.cb` (S2.5) is independent of the rest of S2 and can be pulled
forward if data ingest is the more pressing need.

---

## S1. Foundation

### S1.1 `sci/complex.cb`

The blocking dependency for eigenvalues, polynomial roots, and a decent
FFT surface. `fft.cb` today forces callers into a hand-interleaved
`double[]` of length `2*N` (element k at `buf[2k]`, `buf[2k+1]`), which is
the ugliest edge in the current library.

A plain scalar value type with operator overloads, modelled directly on
`vec2` in `linear_math.cb` (same shape, same style, same reasons):

```cflat
struct complex
{
    double re = default;
    double im = default;

    complex operator+(complex o);
    complex operator-(complex o);
    complex operator-();
    complex operator*(complex o);
    complex operator/(complex o);
    complex operator*(double s);
    complex operator/(double s);
    bool    operator==(complex o);

    double  abs();            // hypot(re, im)
    double  abs2();           // re*re + im*im, no sqrt
    double  arg();            // atan2(im, re)
    complex conj();
    complex reciprocal();
};

namespace Cx
{
    complex make(double re, double im);
    complex polar(double r, double theta);     // r * (cos t + i sin t)
    complex exp(complex z);
    complex log(complex z);
    complex sqrt(complex z);
    complex pow(complex z, complex w);
}
```

Notes:
- `abs()` must use `Math.hypot`, not `sqrt(re*re + im*im)`, or it overflows
  for large components. This is the kind of thing that silently passes a
  naive test and fails on real data.
- Keep it scalar and branch-light like `vec2`, so it inlines. Do NOT add a
  `complex[]` kernel set in this slice; that is an S3 question once `fft.cb`
  is reworked.
- Division needs Smith's algorithm (scale by the larger component) to avoid
  spurious overflow, not the textbook `(ac+bd)/(c*c+d*d)`.

### S1.2 `sci/stats.cb`

The most-reached-for module in any scientific library, and almost entirely
straightforward code over `double[]`.

```cflat
namespace Stats
{
    // Central tendency and spread. mean/variance/stddev delegate to
    // Scan.Welford rather than re-summing (numerically stable, one pass).
    double mean(double[] x, int n);
    double variance(double[] x, int n);          // population
    double sample_variance(double[] x, int n);   // N-1
    double stddev(double[] x, int n);
    double sample_stddev(double[] x, int n);

    // Order statistics. All SORT A CALLER-OWNED SCRATCH COPY - they do not
    // mutate the input, and they do not allocate (scratch is passed in).
    double median(double[] x, int n, double[] scratch);
    double quantile(double[] x, int n, double q, double[] scratch);
    double iqr(double[] x, int n, double[] scratch);

    // Pairwise.
    double covariance(double[] x, double[] y, int n);
    double correlation(double[] x, double[] y, int n);   // Pearson
    double spearman(double[] x, double[] y, int n, double[] scratch);

    // Simple ordinary least squares y = slope*x + intercept.
    struct LinearFit { double slope; double intercept; double r2; };
    LinearFit linfit(double[] x, double[] y, int n);

    // Binning. bins[] is caller-owned, length nbins; counts are i64.
    void histogram(double[] x, int n, double lo, double hi, i64[] bins, int nbins);

    // Normalization, in place, vectorized (these ARE Vec-shaped).
    void zscore(double[] x, int n);
    void minmax_scale(double[] x, int n);
}
```

Notes:
- **Scratch-in, not allocate-in.** Every routine that needs a sorted copy
  takes the buffer from the caller. That keeps the module allocation-free
  on the hot path, matching the stated contract of `vecmath.cb` and
  `solvers.cb`, and sidesteps ownership questions entirely.
- `quantile` should use linear interpolation between order statistics (the
  "type 7" definition, what numpy and R default to), and the choice must be
  stated in the file header comment - quantile definitions differ between
  libraries and a silent choice will bite someone.
- `correlation` computed as `cov/(sx*sy)` can drift outside `[-1, 1]` by an
  ulp; clamp the result.

### S1.3 `random.cb` distributions

`Random` today offers `next`, `nextInt(min,max)`, `nextDouble`, plus
`jump`/`split` for parallel streams. Add, as methods on `Random`:

```cflat
    double nextDouble(double lo, double hi);
    float  nextFloat();
    bool   nextBool();
    bool   nextBool(double p);              // Bernoulli

    double nextGaussian();                  // standard normal
    double nextGaussian(double mean, double stddev);
    double nextExponential(double lambda);
    i64    nextPoisson(double lambda);
    i64    nextBinomial(i64 trials, double p);
    double nextGamma(double shape, double scale);

    void   shuffle(double[] x, int n);      // Fisher-Yates
    void   shuffle(int[] x, int n);
    int    weightedChoice(double[] weights, int n);
```

Notes:
- `nextGaussian` should be **Marsaglia polar**, not Box-Muller: it avoids
  `sin`/`cos`, and its rejection loop is cheap. Cache the second variate in
  a `_spare` field on `Random` - but then `_spare` becomes part of generator
  state, so `split()` and `jump()` MUST clear it or two streams share a
  variate. Call this out in the code; it is an easy bug to ship.
- `nextInt(min, max)` currently uses `next() % range`, which is modulo-
  biased. While in here, fix it to rejection sampling. Note this is a
  **behaviour change**: an existing seeded sequence will produce different
  integers. Flag it for the maintainer rather than sneaking it in - if any
  test or example pins on a seeded sequence, that test changes too.
- `nextPoisson` needs the Knuth multiply method for small lambda and a
  transformed-rejection / normal approximation above ~30, or it loops
  effectively forever on large lambda.

### S1.4 `sci/special.cb`

Stats is half-useless without these, and libm supplies several for free via
`extern` (the pattern `math.cb` already uses at its top).

```cflat
namespace Special
{
    double erf(double x);
    double erfc(double x);
    double erfinv(double x);          // no libm equivalent; rational approx
    double lgamma(double x);          // extern from libm
    double tgamma(double x);          // extern from libm
    double beta(double a, double b);
    double digamma(double x);

    // Distribution CDFs, the actual reason the above exist.
    double normal_pdf(double x, double mean, double stddev);
    double normal_cdf(double x, double mean, double stddev);
    double normal_quantile(double p);         // inverse CDF, via erfinv
    double student_t_cdf(double t, double df);
    double chi2_cdf(double x, double df);
}
```

Notes:
- Verify that `erf`/`erfc`/`lgamma`/`tgamma` are actually exported by the
  CRT on ALL THREE targets before declaring them `extern`. MSVC's UCRT has
  them; confirm the macOS and Linux paths too, since `math.cb` mixes extern
  declarations with `__`-intrinsic wrappers and the split is per-function.
  If any target is missing one, implement it rather than making the module
  Windows-only.
- `lgamma` is not thread-safe in every libc (the `signgam` global). If the
  system one is suspect, implement Lanczos directly - it is ~20 lines and
  removes the question.

---

## S2. Numerical methods and data ingest

Highest "capability per line" in the plan, and the slice that produces a
demo worth showing.

### S2.1 `sci/integrate.cb`

```cflat
namespace Integrate
{
    double trapezoid(Lambda<double(double)> f, double a, double b, int n);
    double simpson(Lambda<double(double)> f, double a, double b, int n);
    double adaptive_simpson(Lambda<double(double)> f, double a, double b,
                            double tol, int max_depth);
    double gauss_legendre(Lambda<double(double)> f, double a, double b, int points);
}
```

Take `Lambda<double(double)>` (the capturing owning closure), not
`function<double(double)>`: an integrand that captures a parameter is the
common case, and a non-capturing lambda converts to `Lambda` anyway.
Confirm that direction of conversion actually holds before committing to
the signature - if it does not, provide both overloads.

`gauss_legendre` needs its nodes/weights. Hard-code the classical 2/4/8/16-
point tables rather than computing them via Newton on Legendre polynomials;
the tables are short, exact, and testable.

### S2.2 `sci/ode.cb`

```cflat
namespace Ode
{
    // Scalar and vector right-hand sides. The vector form writes into a
    // caller-owned dydt[] rather than returning, so stepping allocates nothing.
    double rk4_step(Lambda<double(double, double)> f, double t, double y, double h);

    void rk4_step(Lambda<void(double, double[], double[], int)> f,
                  double t, double[] y, int n, double h, double[] scratch);

    // Dormand-Prince 5(4) adaptive step. Returns the accepted step size;
    // writes the new state into y and the new time into *t.
    double rk45_step(Lambda<void(double, double[], double[], int)> f,
                     double* t, double[] y, int n,
                     double h_try, double abstol, double reltol, double[] scratch);

    // Symplectic velocity-Verlet, for Hamiltonian systems (N-body, orbits)
    // where energy drift over long integrations matters more than local error.
    void verlet_step(Lambda<void(double[], double[], int)> accel,
                     double[] pos, double[] vel, double[] acc, int n, double h);
}
```

Notes:
- Every stepper is allocation-free: stage buffers come from a caller-owned
  `scratch[]`. Document the required scratch length per method in the header
  (`rk4` needs `4*n`, `rk45` needs `7*n`) - getting this wrong is a silent
  out-of-bounds, so assert it where the length is knowable.
- The reason for including Verlet alongside RK is worth stating in the file:
  RK45 is more accurate per step but is NOT symplectic, so a 10-orbit
  simulation visibly spirals. That contrast is what makes a good example.

### S2.3 `sci/roots.cb`

```cflat
namespace Roots
{
    struct Result { double x; int iterations; bool converged; };

    Result bisect(Lambda<double(double)> f, double a, double b,
                  double tol, int max_iter);
    Result newton(Lambda<double(double)> f, Lambda<double(double)> df,
                  double x0, double tol, int max_iter);
    Result secant(Lambda<double(double)> f, double x0, double x1,
                  double tol, int max_iter);
    Result brent(Lambda<double(double)> f, double a, double b,
                 double tol, int max_iter);
}

namespace Optimize
{
    Roots.Result golden_section(Lambda<double(double)> f, double a, double b,
                                double tol, int max_iter);

    // Nelder-Mead over n dimensions. simplex[] is caller-owned,
    // (n+1) * n doubles; the best vertex is written back to x[].
    bool nelder_mead(Lambda<double(double[], int)> f, double[] x, int n,
                     double[] simplex, double tol, int max_iter);
}
```

Return a `Result` with an explicit `converged` flag rather than a bare
`double`. A root finder that silently returns its last iterate on
non-convergence is the classic way a numerical bug escapes into results.

### S2.4 `sci/interp.cb`

```cflat
namespace Interp
{
    double linear(double[] xs, double[] ys, int n, double x);
    double nearest(double[] xs, double[] ys, int n, double x);
    double lagrange(double[] xs, double[] ys, int n, double x);

    // Natural / clamped cubic spline. build() fills a caller-owned
    // coefficient array (4*(n-1) doubles); eval() is then allocation-free.
    void   spline_build_natural(double[] xs, double[] ys, int n, double[] coeffs);
    void   spline_build_clamped(double[] xs, double[] ys, int n,
                                double d0, double dn, double[] coeffs);
    double spline_eval(double[] xs, int n, double[] coeffs, double x);

    double bilinear(double[] grid, int w, int h, double x, double y);
}
```

The spline build solves a tridiagonal system; use the Thomas algorithm
directly rather than pulling in `factor.cb`'s general LU for a tridiagonal
matrix. Locating the interval should be a binary search, not a linear scan,
or `spline_eval` in a loop becomes accidentally quadratic.

### S2.5 `sci/dataio.cb` - pipelined ingest from file

**The problem.** Every kernel in `core/hpc/` takes a `double[]` that is
already in memory, and says nothing about how it got there. The obvious
loader is a serial loop - read a chunk, parse it, read the next - which
leaves the CPU idle for the whole of every read and the disk idle for the
whole of every parse. On a multi-GB CSV or binary trace, that is most of
the wall clock, and it makes the carefully vectorized kernels downstream
irrelevant: the program is bound by an ingest path nobody optimized.

Text parsing is the expensive half. Decimal-to-double conversion runs
tens of nanoseconds per field, so a wide CSV is genuinely CPU-bound on
parse, not I/O-bound - which is exactly why overlapping the two, and then
fanning the parse out across cores, pays.

**The shape.** A three-stage pipeline, each stage already having a primitive
in core:

```
  [read]  -->  [parse]  -->  [consume]
  mmap or       worker         caller's
  chunked       threads        double[] / kernel
  file reads    (parallel.cb)
       \___ SpscQueue / channel handoff, fixed-size buffer pool ___/
```

- **Read stage.** Two backends. The default is double-buffered chunked
  reads through `filesystem.cb` on a dedicated reader thread: while the
  parse stage works on buffer A, the reader is filling buffer B. The second
  is memory-mapped input (the `hpc-gaps.md` G8 work, DONE as of 2026-07-11),
  which removes the copy entirely and lets the parse stage read straight
  from page cache. **Read the G8 benchmark note in `hpc-gaps.md` before
  assuming mmap wins** - it came back NEGATIVE for the single-pass
  streaming case, which is precisely the case here. The honest expectation
  is that mmap helps on re-read and on random access, and that chunked
  reads with prefetch are the right default for a one-pass load. Ship both,
  measure, and record the answer in the file header.
- **Parse stage.** Chunk boundaries do not respect record boundaries, so
  each chunk must be trimmed back to the last complete record and the tail
  carried into the next chunk. Once chunks are record-aligned they are
  independent, and the per-chunk parse fans out over `parallel_for_n` or a
  `ThreadPool` from `parallel.cb`. Output ordering must be preserved: each
  chunk knows its own starting record index and writes to a disjoint slice
  of the destination, so no reordering or locking is needed.
- **Handoff.** `spsc_queue.cb` for the single-reader/single-parser case,
  `channel.cb` when the parse stage is a pool. Buffers come from a fixed
  pool (recycled through a second queue), so the pipeline is
  allocation-free in steady state and has bounded memory regardless of file
  size - a 100 GB file loads in the same footprint as a 100 MB one.

**Proposed surface:**

```cflat
namespace DataIo
{
    // How the reader stage gets bytes. Chunked is the default; Mmap is
    // opt-in and only wins on some access patterns (see notes above).
    enum ReadMode { Chunked, Mmap };

    struct LoadOptions
    {
        ReadMode mode      = default;   // Chunked
        int chunk_bytes    = default;   // 0 -> pick a default (~1 MB)
        int buffers        = default;   // 0 -> 2 (double buffering)
        int workers        = default;   // 0 -> pool default
        char delimiter     = default;   // CSV: 0 -> ','
        bool has_header    = default;
    };

    struct LoadResult
    {
        i64 rows;
        i64 cols;
        i64 bytes_read;
        double seconds;
        bool ok;
    };

    // --- one-shot loads (the 90% case) ---

    // Whitespace/newline-separated doubles into a caller-owned buffer.
    // Returns the count actually read; never allocates the destination.
    LoadResult load_doubles(string path, double[] dst, i64 capacity,
                            LoadOptions opts);

    // Rectangular numeric CSV into a row-major double[] of rows*cols.
    LoadResult load_csv(string path, double[] dst, i64 capacity,
                        i64 expected_cols, LoadOptions opts);

    // Raw little-endian f64/f32 records - no parse stage, so the pipeline
    // degenerates to pure prefetch. Cheap and worth having.
    LoadResult load_binary_f64(string path, double[] dst, i64 capacity,
                               LoadOptions opts);

    // --- streaming (files larger than memory) ---

    // Invokes `sink` once per parsed chunk, IN ORDER, on the calling thread.
    // The parse of chunk N+1 overlaps the sink call for chunk N.
    LoadResult stream_doubles(string path,
                              Lambda<void(double[], int, i64)> sink,
                              LoadOptions opts);

    // Fold a whole file without materializing it: the classic
    // "mean/variance of a file that does not fit in RAM" case. Combines
    // per-chunk Welford accumulators from Scan.
    LoadResult reduce_file(string path, Welford* acc, LoadOptions opts);
}
```

Notes and constraints specific to this module:

- **Correctness first, and it is the hard part.** The chunk-boundary carry,
  a record split across a buffer edge, a file with no trailing newline, CRLF
  vs LF, and a short final chunk are all off-by-one traps. Test these
  explicitly - a pipelined loader that is fast and drops the last row is
  worse than the serial loop it replaced.
- **Fall back to serial for small files.** Below roughly one chunk, thread
  spawn and handoff cost more than the read. Take the single-threaded path
  and say so in the header, so nobody benchmarks a 4 KB file and concludes
  the pipeline is slow.
- **Report, do not guess.** `LoadResult` carries `bytes_read` and `seconds`
  so a caller can compute achieved MB/s. Add a loader row to
  `performance.bat` per the repo rule that benchmarks run at `-O2`, and
  record baseline throughput in `internal/performance-benchmarks.md`
  alongside the existing stream/channel numbers.
- **Pin the benchmark, and A/B in fresh processes.** This host is
  heterogeneous (Zen5 perf cores 0/2/4/6, Zen5c compact cores 8-18, ~2.8x
  L3 latency difference). An unpinned loader benchmark will migrate
  mid-run and produce a ~2x skew that looks like a real result.
- **`double` parsing is the hot loop.** Do not call `sscanf` per field; it
  reparses a format string every time. A direct decimal scanner is several
  times faster, and this is the one place in the module where a hand-rolled
  loop is justified rather than a review defect.
- Error handling goes through `LogError` / `LogErrorContext` with the file
  path and the byte offset of the offending record - "parse error" with no
  position is useless on a multi-GB input.

---

## S3. Linear algebra completion

### S3.1 `hpc/qr.cb`

Householder QR plus linear least squares. Arguably the single most-used
routine in applied numerics, and the missing prerequisite for both eigen
and a properly-conditioned fit on multiple predictors.

```cflat
namespace Qr
{
    // In-place Householder QR of a row-major m x n A (m >= n). Reflector
    // scalars go to tau[n]; R is the upper triangle of A on return.
    void factor(double[] a, int m, int n, double[] tau);

    // Apply Q^T to b in place (the "form the RHS" half of a least squares).
    void apply_qt(double[] a, int m, int n, double[] tau, double[] b);

    // Solve min ||Ax - b||_2 for the overdetermined case. A is destroyed.
    bool lstsq(double[] a, int m, int n, double[] b, double[] x, double[] tau);

    // Explicitly materialize Q (m x n, thin) when the caller needs it.
    void form_q(double[] a, int m, int n, double[] tau, double[] q);
}
```

The trailing update should route through `DenseMat.gemm_ld` at large `n`,
exactly as `factor.cb`'s blocked pivoted LU already does - same pattern,
same reasons.

### S3.2 `hpc/eigen.cb`

There is currently no way to obtain an eigenvalue at all; `factor.cb` stops
at LU/Cholesky.

```cflat
namespace Eigen
{
    // Symmetric case: cyclic Jacobi. Eigenvalues to w[n], eigenvectors to
    // the columns of v[n*n]. A is destroyed. Real, always converges.
    bool jacobi_symmetric(double[] a, int n, double[] w, double[] v,
                          double tol, int max_sweeps);

    // Dominant eigenpair by power iteration (cheap, works on sparse via
    // a caller-supplied matvec).
    double power_iteration(Lambda<void(double[], double[], int)> matvec,
                           double[] v, int n, double tol, int max_iter);

    // General real case: Hessenberg reduction + shifted QR. Eigenvalues are
    // complex in general, so this is what S1.1 was a prerequisite for.
    bool qr_algorithm(double[] a, int n, complex[] w, int max_iter);
}
```

Ship `jacobi_symmetric` and `power_iteration` first; `qr_algorithm` is
substantially harder (deflation, Wilkinson shifts, 2x2 complex blocks) and
can be a follow-on if the symmetric case covers the demand.

### S3.3 `fft.cb` completeness

`fft.cb` is 99 lines and explicitly leaves all of this out:

- `rfft` / `irfft` for real input, roughly 2x faster and by far the common case.
- 2-D FFT (row transforms, transpose, row transforms) - reuse
  `DenseMat.transpose` rather than writing a second one.
- Bluestein's algorithm for arbitrary (non-power-of-two) `n`. Today a
  non-power-of-two length is simply unsupported, which is a sharp edge.
- A precomputed twiddle table. The header currently notes twiddles are
  computed on the fly "to keep the kernel self-contained"; an `FftPlan`
  struct owning the table is the standard fix and a large speedup.
- FFT-based `convolve` / `correlate`.

If S1.1 lands, revisit whether the public surface should move to `complex[]`
and keep the interleaved `double[]` form as the internal representation.
That is an API break for existing callers, so it needs a maintainer ruling;
do not do it silently as part of this item.

---

## S4. Deferred (recorded so it is not re-proposed)

- `sci/poly.cb` - polynomial evaluate/derivative/integrate is trivial, but
  root-finding via the companion matrix needs `Eigen.qr_algorithm` first.
- Dimensional analysis / units. Doing this properly wants compiler support
  (type-level exponents), which puts it in `hpc-gaps.md` territory, not here.
- Sparse direct factorization (sparse Cholesky, symbolic + numeric phases).
  Large, and the Krylov solvers in `solvers.cb` already cover most demand.
- `complex[]` BLAS-1 kernels (`Vec` for complex). Wait until an actual
  caller needs them; speculative kernel surface is how a library rots.
- Async / overlapped file I/O in `dataio.cb` (IOCP, io_uring). The thread +
  queue pipeline in S2.5 gets most of the overlap at a fraction of the
  complexity and stays portable across all three targets. Revisit only if a
  measurement shows the reader thread is the bottleneck.

---

## Constraints (repo rules that apply to every item)

- **Library only.** No grammar, `LLVMBackend`, or `MainListener` change is
  in scope. If an item appears to need one, stop and raise it - that is a
  `hpc-gaps.md` item, not a `sci-library.md` item.
- **New `core/*.cb` files need no project-file edit.** The CMake
  `CONFIGURE_DEPENDS` glob copies `core/` to the exe dir; a rebuild picks up
  a new file automatically. A new SUBDIRECTORY (`core/sci/`) should be
  confirmed against how `core/hpc/` and `core/graphic/` are globbed before
  assuming the same.
- **After editing a `core/*.cb`, rebuild before testing through the exe.**
  The compiler reads the DEPLOYED copy next to it, not the source tree.
  Clearing the cache does NOT help. This has cost real debugging time before.
- **No new test files.** Regression cases extend the existing
  `Test/test_hpc_kernels.cb`, `Test/test_hpc.cb`, and `Test/test_math.cb`.
- **Float literals default to float, not double.** `100.0/99.0` folds in
  single precision and then widens. Expected values in numerical tests must
  come from explicitly-typed `double` variables or be exactly representable,
  or the test asserts the wrong number.
- **ASCII only** in source, comments, and docs. `LogError` /
  `LogErrorContext` for any diagnostic; no `LogWarning`, no `std::cout`.
- **Inline comments 2 lines or fewer**; longer commentary goes above the
  function or at the top of a scope.
- **Do not touch `cflat/locales/`.** Those files are generated.

## Verification

Per slice, on the current host:

- `cmake_build.bat release` (or `cmake_build.sh release` on macOS/Linux),
  then `test.bat` / `./test.sh Release` green.
- `test_lsp.bat` after any change that adds core symbols, since the symbol
  index sweeps `core/`.
- `example.bat` if the slice adds or touches an example.
- Numerical correctness is checked against closed-form or high-precision
  reference values written INTO the test, not against a second
  implementation in the same file (which would only test self-consistency).
- S2.5 additionally: a `performance.bat` loader row at `-O2`, pinned, with
  the achieved MB/s recorded in `internal/performance-benchmarks.md`; and
  boundary tests (record split across chunks, no trailing newline, CRLF,
  short final chunk, file smaller than one chunk).

## Suggested example programs

One per slice, in `example/hpc/` (or `example/sci/` if S1 decision (a) is
taken). These are what make the library legible:

- S1: Monte Carlo estimate of a distribution's tail, cross-checked against
  `Special.normal_cdf` - exercises `Random` distributions, `Stats`, and
  `Special` against each other.
- S2: two-body orbit integrated with RK45 and with Verlet, printing the
  energy drift of each over many orbits. Shows visibly why symplectic
  integrators exist.
- S2.5: load a large generated CSV with the pipeline and with a naive
  serial loop, printing MB/s for each. The point of the example is the
  ratio, and it doubles as the benchmark.
- S3: least-squares polynomial fit to noisy data via `Qr.lstsq`, plus the
  vibration modes of a spring-mass chain via `Eigen.jacobi_symmetric`.

---

## As built (S1 + S2, 2026-08-29)

Delivered on branch `plan/sci-library`, uncommitted. 2067 lines across 8 modules
in `cflat/core/sci/`, plus distribution methods on `Random`.

Gates on the merged tree: `cmake_build.bat release` exit 0; `test.bat Release`
all passed, 0 failed, 0 skipped; `test_lsp.bat Release` all passed;
`test_math.cb` 98/98; `test_hpc_kernels.cb` 85/85.

### Deviations from the sketches above

1. **Closure parameters take `double*`, not `double[]`.** CFlat rejects array
   views inside function types, so every callback signature in `ode.cb`,
   `Optimize.nelder_mead`, and `DataIo.stream_doubles` takes a raw pointer plus
   an explicit length. Two independent implementations hit this separately, so
   it is a language limitation and not an implementation choice. If array views
   in function types are ever supported, these signatures should be revisited.

2. **`Special.erf` / `Special.erfc` forward to libm** (`extern`), rather than
   using a portable polynomial. The first implementation used the
   Abramowitz-Stegun 7.1.26 fit, whose ~1.5e-7 absolute error is single-precision
   accuracy on a double API and leaves `erfc` - and therefore `normal_cdf` -
   meaningless in the tail. libm supplies both on all three targets.
   `Special.lgamma` stays a CFlat Lanczos implementation because the libc
   `lgamma` writes the `signgam` global and is not thread-safe everywhere.
   `Test/test_math.cb` carries relative-error tail assertions that fail against
   the polynomial version; do not replace the extern with an approximation.

3. **`Stats.spearman` is O(n^2)** - `_rank` scans the array per element, which
   is what lets it compute correct midranks for ties from a single scratch
   buffer. Fine for modest n, wrong tool for large arrays. Ranking both inputs
   by sorting would need a second scratch buffer and an API change.

4. **`DataIo.workers` applies only to the whitespace-decimal entry points**
   (`load_doubles`, `stream_doubles`, `reduce_file`). `load_csv` and
   `load_binary_f64` parse each chunk serially and ignore it.

5. **A single record longer than `chunk_bytes` is refused**, not split: the
   reader sets an overflow flag and the load returns `ok = false` rather than
   silently mis-parsing.

6. **`ReadMode.Mmap` does not bound memory** - it hands the whole mapping to the
   parser in one call. It stays opt-in, and `Chunked` remains the default, which
   is the behaviour the G8 benchmark in `hpc-gaps.md` argues for anyway.

### Verified, not merely claimed

The S2.5 pipeline was proven rather than asserted, via `scratch/proof_dataio.cb`
(gitignored; it generates and then deletes its own 50 MB input):

- **Bounded memory**: streaming a 50 MB file with `chunk_bytes = 1 MB` and
  `buffers = 2` moves process peak working set from 4 MB to 7 MB. A
  whole-file-into-RAM implementation moves it by ~55 MB.
- **Real concurrency**: with only the reader stage started, the main thread
  sleeps 300 ms consuming nothing and finds exactly 2097152 bytes read - the
  2-buffer pool, filled and then blocked on the free ring.
- **`buffers` and `workers` change behaviour**: peak working set tracks
  `buffers * chunk_bytes` (7 MB -> 13 MB going 2 -> 8), and `workers = 4`
  measurably reduces wall time.
- **Boundary cases all pass**, and are permanent regressions in
  `Test/test_hpc_kernels.cb` (`test_dataio_pipeline`) with `chunk_bytes = 32` so
  records straddle chunk edges by construction: record split across a chunk
  edge, no trailing newline, CRLF, short final chunk with an unterminated last
  record, file smaller than one chunk, empty file.

This proof obligation exists because a first implementation passed the entire
suite while calling `File.readAllText` and doing no pipelining at all. A test
that only checks parsed values cannot tell the two apart.

### Still open for a maintainer ruling

- **`Random.nextInt(min, max)` remains modulo-biased.** Fixing it to rejection
  sampling changes every existing seeded sequence, so it was deliberately left
  alone. It is a behaviour change, not a bug fix, and should be taken
  separately.

---

## As built (S3, 2026-08-29)

Delivered on the same branch, uncommitted. 1392 lines across three modules:
`cflat/core/hpc/qr.cb` (314 new), `cflat/core/hpc/eigen.cb` (545 new), and
`cflat/core/hpc/fft.cb` (99 -> 533).

Gates on the fully merged S1+S2+S3 tree: `cmake_build.bat release` exit 0;
`test.bat Release` all passed, 0 failed, 0 skipped; `test_lsp.bat Release` all
passed; `example.bat Release` 100 passed, 0 failed, 41 skipped;
`test_math.cb` 98/98; `test_hpc.cb` 518/518 (was 304); `test_hpc_kernels.cb`
139/139 (was 85).

### Everything in S3 shipped

Both items the plan flagged as possibly-too-hard were completed rather than
deferred: `Eigen.qr_algorithm` (Francis implicit double shift, Householder
Hessenberg reduction, bulge chase, exceptional shift on stagnation) and
Bluestein's algorithm for arbitrary non-power-of-two FFT lengths.

### Deviations from the sketches above

1. **`Eigen.power_iteration` takes a trailing `bool* converged`.** The sketch
   returns a bare `double`, which cannot report non-convergence.
2. **`Qr.factor` gained optional `ThreadPool* pool = nullptr, int workers = 0`**,
   matching `factor.cb`. Used only by the blocked gemm trailing update, and
   verified bit-identical to the serial path.
3. **`Eigen.jacobi_symmetric` sorts eigenvalues ascending** (LAPACK convention),
   carrying the eigenvector columns with them.
4. **RESOLVED** - `eigen.cb` imported `sci/complex.cb`, an hpc -> sci dependency
   the plan implies by giving `qr_algorithm` a `complex[]` output, inverting the
   layering under Open decision 1. Ruling: the import system is a module system,
   so the graph must stay a DAG, and shared items move into their own file.
   `complex` was never science-specific, so it now lives at the core root beside
   `math.cb` as `complex.cb`; `sci/complex.cb` is gone rather than left as a
   second path to the same module. Guarded by "eigen does not import sci/" in
   `Test/test_hpc_kernels.cb`. Verified at the time of the change: 107 modules,
   0 cycles, 0 inversions.
5. **`fft.cb` now imports `hpc/densemat.cb`** for `Mat.transpose` in the 2-D
   transform, as the plan required ("reuse `DenseMat.transpose` rather than
   writing a second one"). That transitively pulls in `threadpool.cb` and
   `hpc/parallel.cb`, so `fft.cb` is no longer the self-contained kernel its
   original header advertised. Nothing spawns a thread unless `gemm` is called.
   Reverting to a local transpose would restore self-containment at the cost of
   the guiding principle; maintainer call.
6. **`FFT.fft_any` / `ifft_any` are out-of-place** `(src, out, n)` while
   `FFT.fft` / `ifft` are in-place `(buf, n)`. Bluestein genuinely needs
   auxiliary buffers so out-of-place is natural, but the asymmetry will surprise
   callers and is worth a second look.
7. The existing FFT public surface is otherwise **unchanged** and was not
   migrated to `complex[]`, per the plan's instruction that this needs a
   maintainer ruling. The implementing agent's view, recorded for that decision:
   the interleaved form is the better internal representation (both the
   real-input half-length trick and the transpose step index raw doubles), so a
   migration should be a thin `complex[]` facade over these kernels rather than
   a rewrite.
8. **`QR_BLOCK_MIN_N = 96`** is measured, not guessed: at -O2 on m = 1.5n,
   blocked QR is ~1.4x SLOWER at n=48-64, a wash at 96, 1.8x faster at 128, and
   4-10x from 256 up. The measurement is recorded in the constant's comment.

### Known accuracy limits (stated, not hidden)

- **`power_iteration` can return the subdominant eigenvalue** and still report
  `converged = true`, when the start vector is exactly orthogonal to the
  dominant eigenvector. This is inherent to the method - no implementation can
  detect it from matvecs alone. It is documented on the function and pinned as
  its own regression check rather than papered over.
- **Defective (non-diagonalizable) eigenvalues lose ~2/3 of the digits**: a 3x3
  Jordan block for eigenvalue 2 resolves to 7.4e-6, which is `eps^(1/3)` - the
  conditioning of a defective triple root, not an implementation defect.
- **`lstsq` on an ill-conditioned system loses digits as the conditioning
  predicts**: ~7.8e-10 on a near-collinear tall system with cond ~1e8, 8.6e-13
  on a degree-7 Vandermonde over 20 points.

### Independently verified by the main session, not merely reported

- **Bluestein** checked against a DFT written from scratch in the review probe
  (`scratch/probe_bluestein.cb`), sharing no code with `fft.cb`, at n =
  3,5,6,7,11,12,13,17,31,100. Max error 2.9e-11, most sizes at 1e-15.
- **Eigen** checked against spectra known in closed form
  (`scratch/probe_eigen.cb`): a diagonal matrix (exact, 0.0), the (2,-1)
  symmetric tridiagonal at n=12 against `2 - 2cos(k*pi/(n+1))` (8.9e-16, with
  `V^T V - I` at 2.0e-15), and `qr_algorithm` on the companion matrix of
  `(x-2)(x-4)(x+3)(x-1)` (3.1e-15, zero spurious imaginary part).
- The FFT suite was **mutation-tested** by its implementing agent: flipping the
  plan twiddle sign, flipping the `rfft` untangle sign, shortening the Bluestein
  convolution to `n`, and dropping the second transpose each produced 2-18
  failures. The tests are not vacuous.

### Note on agent-reported LSP failures

Both Codex runs and the S3.1/S3.2 agent reported `test_lsp.bat` failing on
`viewAssembly: inline attribution` ("server died"). It does **not** reproduce:
`test_lsp.bat Release` passes on the merged tree, and passed on two consecutive
runs on the S3 tree. The likely cause is machine load - those reports all came
from sessions running concurrently with other agents and builds - not a code
change. Do not chase it as a regression from this work.
