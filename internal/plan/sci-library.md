# Sci library: as-built record

Status: **DONE and committed** (`b3ff9ee`, "Add science library to core"). Consolidated
2026-08-31 from `sci-library.md` (S1-S3) and `sci-library-gap.md` (G1-G5, the SciPy-gap
follow-on); both plans were landed records by then. Companion: `internal/plan/hpc-gaps.md`
covers the COMPILER/runtime gaps (SIMD, alignment, parallel scheduling); this work needed no
grammar, backend or codegen change.

The sketches that preceded implementation are gone. What survives here is the shipped
surface, what stayed deferred, the one open ruling, the accuracy limits, and the defects the
build found - the last because they are the reason the verification discipline below exists.

## Shipped surface

`cflat/core/sci/`: `complex.cb`, `stats.cb`, `special.cb`, `integrate.cb`, `ode.cb`,
`roots.cb`, `interp.cb`, `dataio.cb` (pipelined file ingest), `distributions.cb`,
`constants.cb`, `optimize.cb`, `signal.cb`, `spatial.cb`, `cluster.cb`. Distribution methods
were extended into `random.cb` in place rather than wrapped.

`cflat/core/hpc/`: `qr.cb` (+ lstsq), `eigen.cb`, `svd.cb`, Bluestein FFT completeness in
`fft.cb`, and `vecmath.cb`.

S1+S2 alone were 2067 lines across 8 modules. Rulings taken at the start and now closed:
`core/sci/` as the directory, lowercase `complex`, distributions extended into `random.cb`.

Gates at merge: `test.bat Release` all passed 0 skipped, `test_lsp.bat Release` all passed,
`test_example.bat Release` 100 passed / 0 failed / 41 skipped, `test_math.cb` 98/98,
`test_hpc_kernels.cb` 85/85.

## Still open: one maintainer ruling

**`Random.nextInt(min, max)` remains modulo-biased.** Fixing it to rejection sampling changes
every existing seeded sequence, so it was deliberately left alone. That is a behaviour
change, not a bug fix, and should be taken as its own item.

## Deferred by design (recorded so it is not re-proposed)

- `sci/poly.cb` - evaluate/derivative/integrate is trivial, but companion-matrix root finding
  needs `Eigen.qr_algorithm` first.
- Dimensional analysis / units. Doing it properly wants compiler support (type-level
  exponents), which puts it in `hpc-gaps.md` territory, not here.
- Sparse direct factorization (sparse Cholesky, symbolic + numeric phases). Large, and the
  Krylov solvers in `solvers.cb` already cover most demand.
- `complex[]` BLAS-1 kernels (`Vec` for complex). Wait for an actual caller; speculative
  kernel surface is how a library rots.
- Async / overlapped file I/O in `dataio.cb` (IOCP, io_uring). The thread + queue pipeline
  gets most of the overlap at a fraction of the complexity and stays portable. Revisit only
  if a measurement shows the reader thread is the bottleneck.

## Known accuracy limits (stated, not hidden)

- **`power_iteration` can return the subdominant eigenvalue** and still report
  `converged = true`, when the start vector is exactly orthogonal to the dominant
  eigenvector. Inherent to the method - no implementation can detect it from matvecs alone.
  Documented on the function and pinned as its own regression check rather than papered over.
- **Defective (non-diagonalizable) eigenvalues lose ~2/3 of the digits**: a 3x3 Jordan block
  for eigenvalue 2 resolves to 7.4e-6, which is `eps^(1/3)` - the conditioning of a defective
  triple root, not an implementation defect.
- **`lstsq` on an ill-conditioned system loses digits as the conditioning predicts**: ~7.8e-10
  on a near-collinear tall system with cond ~1e8, 8.6e-13 on a degree-7 Vandermonde over 20
  points.

## Constraints that shaped the code (still apply to anything added here)

- CFlat rejects array views (`double[]`) inside function types, so every callback signature
  takes `double*` plus an explicit length.
- Float literals default to **float**, not double. A literal-only expression such as
  `100.0 / 99.0` folds in single precision before widening. Precompute such constants as
  decimal literals, or force a double operand.
- Hot paths take caller-owned scratch and do not allocate per call. Where a routine documents
  a scratch size, that contract is load-bearing and is asserted by a test.
- No new test files; additions go into `Test/test_math.cb` and `Test/test_hpc_kernels.cb`.
- After editing any `core/*.cb`, rebuild before testing through the exe - the compiler reads
  the DEPLOYED `core/` next to the binary, not the source tree.

## Dependency-closure result

`hpc/fft.cb` needed exactly one thing from `hpc/densemat.cb` - a transpose for the 2-D
transform - and paid for it with that module's entire threading stack (`threadpool.cb`,
`hpc/parallel.cb` and their closures). `Vec.transpose` now lives in `hpc/vecmath.cb`, a leaf
module whose only import is `math.cb`; `Mat.transpose` forwards to it, so the public API is
unchanged.

| module | transitive imports before | after |
|--------|---------------------------|-------|
| `hpc/fft.cb`    | 18 | 2 |
| `sci/signal.cb` | 19 | 3 |

Guarded by `test_import_closure()` in `Test/test_hpc_kernels.cb` - see "Dependency-closure
guard" in `internal/testing-notes.md`.

## Why every slice was probed independently

Numerical code is the worst case for self-consistent testing: a wrong formula and a test
written from the same wrong formula agree perfectly. The rule that came out of this work is
that each slice is probed in `scratch/` against closed form, or against a reference written
from scratch inside the probe, **never** against the implementation under test. An earlier
round shipped three defects that passed the whole suite (a fake pipeline, a worthless `erf`
tail, minified source).

What that discipline actually caught - none of it visible to the suite as delivered:

- **SVD (G1).** The first agent run produced a module that had never been executed: it failed
  22 consecutive times trying to write the file (a base64 git patch piped through nested
  `pwsh -Command`, which the Windows sandbox rejects) and its patch header was malformed. The
  source was recovered from the agent log and handed to the next run as SUSPECT code with
  specific questions - does the QR sweep converge, is `vt` really V transposed, do the
  allocating paths free what they take, is the documented `m*n + n` scratch length what the
  code touches. It found and fixed **six** defects.
- **Clustering (G5).** `Cluster.linkage` was correct for `LINKAGE_SINGLE` and wrong for
  `LINKAGE_COMPLETE`, `LINKAGE_AVERAGE` and `LINKAGE_WARD`. Five points at x = 0, 1, 4, 9, 10
  make the hierarchy unambiguous, so the merge-size sequence must be `2, 2, 3, 5` for every
  method; three methods produced `2, 3, 4, 5`.
- **Signal (G4).** `butter_highpass` was wrong by 3.7% at its own cutoff. The lowpass-to-
  highpass transform maps a prototype pole `p` to `Omega_c / p`, not `Omega_c * p`; the code
  reused a value that was only correct in magnitude for unit-circle prototype poles, leaving
  the pole radius at 1 instead of the warped cutoff.
- **Optimize (G3).** The `converged` flag had to be corrected twice. Round one removed a
  hardcoded `grad_norm <= 0.0001` that ignored the caller's `gtol`; round two found the same
  defect reintroduced inside a wrapper, `curve_fit` passing `gtol * 1000.0` down to
  `least_squares` - harder to spot because it scales with the caller's own argument and so
  reads as deliberate. The structural pressure behind it (an early return on
  `!fit.converged` meant an accurate fit that merely missed a tight `gtol` produced no
  covariance) was removed too: `covariance_valid` now comes from whether the normal matrix
  inverts, independently of `converged`, matching SciPy.

Positive verification worth keeping: Bluestein was checked against a from-scratch DFT sharing
no code with `fft.cb` at n = 3,5,6,7,11,12,13,17,31,100 (max error 2.9e-11); eigen against
closed-form spectra (diagonal exact; the (2,-1) tridiagonal at n=12 against
`2 - 2cos(k*pi/(n+1))` at 8.9e-16, `V^T V - I` at 2.0e-15; `qr_algorithm` on the companion
matrix of `(x-2)(x-4)(x+3)(x-1)` at 3.1e-15 with no spurious imaginary part). The FFT suite
was mutation-tested - flipping the plan twiddle sign, flipping the `rfft` untangle sign,
shortening the Bluestein convolution to `n`, and dropping the second transpose each produced
2-18 failures, so the tests are not vacuous.

## One false alarm, recorded so it is not chased again

Both Codex runs and the S3.1/S3.2 agent reported `test_lsp.bat` failing on
`viewAssembly: inline attribution` ("server died"). It does **not** reproduce: the suite
passes on the merged tree and passed on two consecutive runs on the S3 tree. The reports all
came from sessions running concurrently with other agents and builds. Machine load, not a
regression from this work.
