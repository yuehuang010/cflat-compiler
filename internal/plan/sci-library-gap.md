# sci library: closing the SciPy gap

Status: **PROPOSED** (2026-08-29), branch `plan/sci-library` (base `399ea7c`).

Follow-on to `internal/plan/sci-library.md` (S1-S3 landed as `399ea7c`). That work
covered special functions, quadrature, ODE steppers, 1-D root finding,
interpolation, descriptive statistics, complex numbers, and a pipelined data
reader, plus QR / eigen / Bluestein FFT in `core/hpc/`.

This plan closes the remaining distance to SciPy's commonly-used surface.

## Gap analysis

Ranked by how much each is missed in practice:

| # | SciPy module | Gap | Slice |
|---|---|---|---|
| 1 | `optimize` | no multivariate minimization, no least-squares, no curve fitting | G3 |
| 2 | `stats` distributions | no uniform `pdf/cdf/ppf/rvs` interface; no inverse CDF at all | G2 |
| 3 | `linalg.svd` | no SVD, so no pseudoinverse, rank, or conditioning | G1 |
| 4 | `signal` | nothing: no filter design, filtering, PSD, or peak finding | G4 |
| 5 | `spatial` | nothing: no distance metrics, no KDTree, no hull | G5 |
| 6 | `constants`, `cluster` | nothing; both small | G2, G5 |

**Deliberately out of scope: `ndimage`.** N-dimensional image filtering is a
large surface that buys little without an image type to hang it on, and
`hpc/stencil.cb` already covers separable neighbourhood filtering. If it is
wanted later it should be its own plan.

## Slices

Each slice adds new files only, plus test additions to one existing test file.
No new test files (repo rule). Slices are grouped into waves so that no two
concurrent slices touch the same file.

### G1 - `core/hpc/svd.cb` (wave 1, tests in `Test/test_hpc_kernels.cb`)

Golub-Kahan-Reinsch: Householder bidiagonalization, then implicit-shift QR on
the bidiagonal form. Public surface: `svd`, `singular_values`, `pinv`,
`lstsq` (SVD-based, rank-deficient tolerant), `rank`, `cond`.

Rationale for going first: SVD is the numerically sound basis for
least-squares, and G3 wants it.

### G2 - `core/sci/distributions.cb` + `core/sci/constants.cb` (wave 1, tests in `Test/test_math.cb`)

One uniform interface per distribution - `pdf`/`pmf`, `cdf`, `ppf`, `rvs` -
over: uniform, normal, lognormal, exponential, gamma, beta, chi2, student_t, F,
poisson, binomial, geometric. `ppf` is the headline: it is what unlocks
confidence intervals and inverse-transform sampling, and it is what
`sci/special.cb`'s regularized gamma/beta already make reachable.

`constants.cb` is CODATA physical constants plus unit conversions.

### G3 - `core/sci/optimize.cb` (wave 2, tests in `Test/test_hpc_kernels.cb`)

BFGS and L-BFGS `minimize` with a strong-Wolfe line search, Levenberg-Marquardt
`least_squares`, and `curve_fit` on top of it. Numerical gradient/Jacobian
helpers for callers without analytic derivatives.

Also **relocates** `namespace Optimize` (currently `nelder_mead`, sitting in
`sci/roots.cb`) into this file, so the namespace has one home. `roots.cb` keeps
only `namespace Roots`.

### G4 - `core/sci/signal.cb` (wave 2, tests in `Test/test_math.cb`)

Window functions, `convolve`/`correlate`, `lfilter`/`filtfilt`/`sosfilt`,
Butterworth design via analog prototype + bilinear transform, `firwin`,
`welch` PSD (over `hpc/fft.cb`), `find_peaks`, `detrend`.

### G5 - `core/sci/spatial.cb` + `core/sci/cluster.cb` (wave 3, tests in `Test/test_math.cb`)

Distance metrics (euclidean, sqeuclidean, manhattan, chebyshev, minkowski,
cosine, hamming), `pdist`/`cdist`, a KDTree with k-nearest and radius queries,
and 2-D convex hull (monotone chain). `cluster.cb` adds k-means with k-means++
seeding and hierarchical agglomerative linkage, riding on those metrics.

## Constraints (apply to every slice)

- ASCII only in source, comments and docs.
- Inline comments 2 lines or fewer; longer comments go above a function or at
  the start of a new scope.
- CFlat rejects array views (`double[]`) inside function types: callbacks take
  `double*` plus an explicit length.
- Float literals default to **float**, not double. A literal-only expression
  such as `100.0 / 99.0` folds in single precision before widening. Precompute
  such constants as decimal literals, or force a double operand.
- Hot paths take caller-owned scratch and do not allocate per call. Where a
  routine documents a scratch size, that contract is load-bearing and is
  asserted by a test.
- No new test files. No edits under `cflat/locales/` (generated).
- After editing any `core/*.cb`, rebuild before testing through the exe - the
  compiler reads the DEPLOYED `core/` next to the binary, not the source tree.

## Verification

Beyond the in-suite checks, every slice is independently probed in `scratch/`
against values from closed form or an from-scratch reference written in the
probe itself - never against the implementation being tested. The prior round
shipped three defects that passed the whole suite (a fake pipeline, a worthless
`erf` tail, minified source), so self-consistency is not accepted as evidence.

Gates: `test.bat Release`, `test_lsp.bat Release`, `example.bat Release`.

## Examples

Two to three programs under `example/sci/`, joining `kepler_orbit.cb`. Chosen
so each exercises several of the new modules together rather than demoing one
call.

---

## As built: G2 (distributions + constants) - DONE 2026-08-29

`core/sci/distributions.cb` (572 lines, `namespace Dist`) and
`core/sci/constants.cb` (104 lines, `namespace Const`). All 12 distributions
carry `pdf`/`pmf`, `cdf`, `ppf` and `rvs`. `Test/test_math.cb` grew 97 lines;
it reports 153/153.

### Deviation: `sci/special.cb` was modified

Not in the slice, but justified. `Special.normal_quantile` was
`sqrt(2) * erfinv(2p-1)`, and the existing `erfinv` is a rational
approximation good to roughly 1e-9 - not enough for the round-trip bar. It was
replaced with a bracketed bisection on `normal_cdf`, and `normal_cdf` itself
now evaluates `1 - 0.5*erfc(z)` for `z >= 0` rather than `0.5*erfc(-z)`.
Taking the accuracy hit in the shared function was the right call over
loosening the slice's tolerance.

Three defects in that rewrite were found in review and fixed:

- It returned `1.0e308` where it meant infinity. Now `1.0/0.0`, matching the
  `Dist._infinity()` convention in the same change.
- A dead `else` branch that could never run, since `p > 0.5` had already been
  reflected away.
- A fixed 180-iteration bisection with no early exit, mirrored by a fixed
  200-iteration loop in `Dist._continuous_ppf`. Each iteration costs an `erfc`,
  or a full incomplete beta/gamma for the generic path, so `beta_ppf` was
  paying ~200 continued-fraction evaluations per call. Both now break once the
  bracket reaches one ulp. Verified value-for-value identical afterwards.

### Independent verification

`scratch/probe_dist.cb` checks against closed forms and published critical
values chosen in the probe, sharing no code with `Test/test_math.cb`:
Phi at -1/-2/-3/-6, z(0.975) and z(0.995), chi2 df=2 against
`1 - exp(-x/2)`, gamma(2,1) against `1 - exp(-x)(1+x)`, beta(2,3) against
`6x^2-8x^3+3x^4`, Cauchy `ppf(0.75) == 1`, t(10) and F(5,10) critical values,
and exact rationals for the binomial and geometric. All pass.

Worst `ppf(cdf(x))` round trip over the meaningful range: **1.11e-15**.

**The right tail is bounded by representation, not by the library.** The left
tail round-trips exactly (0.0 relative error at x = -8 and -6). At x = +6 the
error is 1.5e-9 and at x = +8 it is 1.1e-3, both matching the predicted floor
`ulp(1) / pdf(x) / x` to within a factor of two: a cdf that has rounded to
`1 - 9.9e-10` has already destroyed the information before `ppf` is called.
Any implementation has this floor; the fix, if a caller needs that region, is a
survival-function entry point (`sf`/`isf`), which is NOT currently provided.

### Note for a future slice

`Dist` has no `sf`/`isf` (survival function and its inverse). SciPy provides
them precisely for the right-tail case above. Worth adding if p-values in the
far tail are ever needed - the chi-square goodness-of-fit example will use
`1 - cdf`, which is accurate enough at ordinary significance levels but not
beyond about 1e-10.

## As built: G1 (SVD) - DONE 2026-08-29

`core/hpc/svd.cb` (513 lines, `namespace Svd`): Golub-Kahan-Reinsch, with
`svd`, `singular_values`, `rank`, `cond`, `pinv` and `lstsq`.
`Test/test_hpc_kernels.cb` grew 365 lines and 35 SVD checks.

### The first agent run produced code that had never been executed

It generated the full module but then failed 22 consecutive times trying to
write it, because it chose to pipe a base64-encoded git patch through a nested
`pwsh -Command "pwsh -Command \"...\""`; the Windows sandbox rejects that shape
with `CreateProcessWithLogonW failed: 2`. Its patch header was also malformed
(`@@ -0,0 +1,514` with no closing `@@`).

The source was recovered by decoding the base64 out of the agent log and
rebuilding the file from the patch body. The follow-up run was then given the
file as SUSPECT code rather than as finished work, and told to verify what the
first run could not: whether the QR sweep converges at all, whether `vt` is
really V transposed, whether the allocating paths free what they take, and
whether the documented `m*n + n` scratch length is what the code actually
touches. **It found and fixed six defects** - QR state updates, reflector
indexing, U accumulation and pseudoinverse indexing among them.

Lesson worth keeping: an agent that reports a module as written may never have
run it once. Ask what the code does, not what the report says.

### Independent verification

`scratch/probe_svd.cb`, sharing no code with the test file. All 17 checks pass.

- Built A from KNOWN factors - `U0 = R(40deg)`, `V0 = R(25deg)`, `s = (5, 2)` -
  so U, s and V are all known before the call. Singular values recovered to
  8.9e-16.
- **The convention test.** Rotations were chosen specifically because they are
  not symmetric, which is what makes "is `vt` V or V-transposed" decidable:
  taking rows of `vt` as `v_i` satisfies `A v_i = s_i u_i` to 6.7e-16, while
  taking columns violates it by 3.4. Had a symmetric V been used the check
  would have passed either way and proved nothing. The probe asserts the
  column reading FAILS, so the test cannot go vacuous.
- `U^T U == I` to 8.9e-16, `V V^T == I` to 3.9e-16, `U S V^T == A` to 2.7e-15
  on a 7x4 matrix. Input matrix confirmed unmodified.
- Rank-deficient least squares against a closed-form answer: with column 2 =
  2 * column 1 and b = (1,2,3), the minimum-norm solution is exactly
  (0.4, 0.8). Recovered to 1.7e-16, residual exactly the true minimum, and
  smaller in norm than the alternative exact solution (2, 0). This is the
  case `Qr.lstsq` cannot handle.
- Moore-Penrose identities `A A+ A == A` and `A+ A A+ == A+` to 1.3e-15.
- `rank` and `cond` checked against the DOCUMENTED rule (`n * eps * s[0]`)
  computed in the probe, not against a guessed threshold. A diagonal matrix
  with a 1e-17 entry returns that value exactly and is correctly excluded.
- `m < n` is rejected rather than returning garbage.

## As built: G4 (signal) - DONE 2026-08-29

`cflat/core/sci/signal.cb`, 732 lines, `namespace Signal`. Windows (boxcar,
Hann, Hamming, Blackman), `periodogram`, `welch`, `csd`, `coherence`,
`lfilter`, `filtfilt`, `convolve`, `correlate`, `detrend`, `resample_poly`,
`find_peaks`, and Butterworth design (`butter_lowpass`, `butter_highpass`,
`butter_bandpass`, plus second-order-section forms).

Filters are designed as analog prototypes and mapped to discrete time by the
bilinear transform with frequency prewarping. Cutoffs are normalised to
Nyquist = 1, matching SciPy's `Wn`. A bandpass of order N has degree 2N, so
`b` and `a` each need `2*N+1` coefficients - this was undocumented and is now
stated at the function.

### A real defect the full suite did not catch

`butter_highpass` was wrong by 3.7% at its own cutoff frequency.

The lowpass-to-highpass spectral transform maps a prototype pole `p` to
`Omega_c / p`, not `Omega_c * p`. The code computed the analog pole as
`warped / p_lp`, which is `Omega_c / p_lp` in magnitude only when `|p_lp| = 1`
- true for the unit-circle prototype poles, but the code then reused that
value where `Omega_c^2 / p_lp` was required, leaving the pole radius at 1
instead of the warped cutoff. Fixed in both `_butter_denominator` and
`_butter_sos_build`:

```
pwr =  warped * warped * pr / pd;
pwi = -warped * warped * pi / pd;
```

**Why 197 passing tests missed it.** The only highpass assertion checked the
gain at Nyquist, where the design is normalised to 1.0 by construction - so
it reads 1.0 whatever the poles are. The response was only wrong in between.
The new test asserts the -3 dB point is at the requested cutoff, across
several cutoffs and orders. Mutation-tested: reintroducing the bug drops the
file to exactly 169/170, so the test is not vacuous.

### Independent verification

`scratch/probe_signal.cb`, sharing no code with the test file.

- Welch PSD scaling checked by Parseval against a known-variance signal, with
  `fs` and `nperseg` deliberately DIFFERENT (with `fs == nperseg` the bin
  spacing is 1.0, which hides any confusion between the two in the scaling).
- `filtfilt` confirmed zero-phase by checking a symmetric input maps to a
  symmetric output, not merely by checking the magnitude.
- Butterworth magnitude response checked against the closed form
  `1 / sqrt(1 + (w/wc)^(2n))` for low- and highpass at several orders.

Two of my own probe failures were MY error, not the library's: `Math.exp(-1.0)`
folds in single precision (the documented float-literal trap), and the bandpass
arrays were sized `4*order+1` instead of `2*order+1`, so the probe read
uninitialised memory. The latter is what surfaced the missing length
documentation.

## As built: G5 (spatial + cluster) - 2026-08-29

`cflat/core/sci/spatial.cb` (`namespace Spatial`) and `cflat/core/sci/cluster.cb`
(`namespace Cluster`). Points are row-major `double[]` of `npoints * ndim`.

`spatial.cb` verifies clean against `scratch/probe_spatial.cb`, which shares no
code with the test suite:

- Eight metrics plus a `distance(metric, ...)` dispatcher, checked against
  hand-computed values on a 3-4-5 triangle, including the identities
  `euclidean == sqrt(sqeuclidean)`, `minkowski(1) == manhattan`,
  `minkowski(2) == euclidean`, and `minkowski(60)` approaching chebyshev.
- `pdist` produces the condensed upper triangle; every one of the 15 pairs at
  n = 6 was checked to be reachable through `condensed_index` and to equal the
  corresponding `cdist` entry. A condensed layout callers cannot index is
  useless, so this is the check that matters.
- `KdTree` k-nearest and radius queries match an exhaustive scan written inside
  the probe, for k = 1 and k = 5, and for radii capturing none, some and all.
  **It genuinely prunes**: a k=1 query over 2000 points visits 23 of 255 nodes
  (9%). Without that check the tree could be a linear scan with extra steps and
  every other assertion would still pass.
- `convex_hull_2d` returns the 4 corners of a square with interior points, in
  counter-clockwise order (verified by signed area = +16), starting at the
  lowest-then-leftmost point, with every non-hull point strictly left of every
  edge. Collinear input returns 2 vertices, not a degenerate triangle.
- `kmeans` recovers three separated blobs exactly, compared by GROUPING rather
  than by literal label value. Reported inertia matches recomputation from the
  returned centroids, and equals the analytic value of 15. Same seed reproduces;
  asking for more clusters than distinct points produces no NaN centroid.

### Defect: three of the four linkage methods were wrong

`Cluster.linkage` was correct for `LINKAGE_SINGLE` and wrong for
`LINKAGE_COMPLETE`, `LINKAGE_AVERAGE` and `LINKAGE_WARD`.

On five points at x = 0, 1, 4, 9, 10 the hierarchy is unambiguous, so the merge
SIZE sequence must be `2, 2, 3, 5` for every method. Measured:

| method | distances | sizes |
|---|---|---|
| single (correct) | 1, 1, 3, 5 | 2, 2, 3, 5 |
| complete (want 1, 1, 4, 10) | 1, 5, 6, 9 | 2, 3, 4, 5 |
| average (want 1, 1, 3.5, 47/6) | 1, 5.5, 7, 8.25 | 2, 3, 4, 5 |
| ward | 1, 6.62, 8.25, 8.89 | 2, 3, 2, 5 |

Sizes `2, 3, 4, 5` are a CHAIN - every step merges the cluster just created
with one more singleton, which is what a mis-indexed Lance-Williams update
produces. Single linkage survived because its update is `min(d_ik, d_jk)`, the
one rule where reading a stale neighbour distance can still give the right
answer.

**Why the suite missed it.** The in-suite test exercised complete and average
linkage on THREE points and asserted only that the methods "differ". With
n = 3 there are just two merges and the size sequence is `2, 3` for every
possible tree, so the chain bug cannot appear. n >= 5 with two well-separated
pairs is the smallest case that separates the methods structurally.

Worth keeping: "differs from single" is not evidence of correctness. My own
probe's `complete linkage differs from single` check passed while complete
linkage was wrong.

## As built: G3 (optimize) - DONE 2026-08-29

`cflat/core/sci/optimize.cb`, `namespace Optimize`: `minimize_bfgs`,
`minimize_lbfgs`, `numeric_gradient`, `numeric_jacobian`, `least_squares`
(Levenberg-Marquardt), `curve_fit`, and `nelder_mead` relocated here from
`roots.cb`.

Took two remediation rounds. The first round's in-suite tests passed 197/197
while every one of the following was true, all measured by
`scratch/probe_optimize.cb`:

| case | before | after |
|---|---:|---:|
| BFGS on 2-D Rosenbrock | 223 iters / 1570 evals | 35 / 59 |
| L-BFGS on 2-D Rosenbrock | delegated to BFGS | 39 / 118 |
| L-BFGS on 10-D Rosenbrock | did not converge in 4000 | 146 / 179 |
| ill-conditioned quadratic (n=6) | 418 iters | 7 / 58 |
| least_squares on a LINEAR model | error 1e-8, cost 2.7e-17 | exact, cost 0 |

The fixes were a real strong-Wolfe search with interpolating zoom, removal of
the `n <= 2` delegation that made `minimize_lbfgs` a synonym for
`minimize_bfgs`, and Levenberg-Marquardt damping that decays far enough to
solve a linear problem in one step.

### The `converged` flag had to be corrected twice

Round one replaced a hardcoded `if (grad_norm <= 0.0001) converged = true`
that ignored the caller's `gtol`. Round two found the same defect reintroduced
in a new place: `curve_fit` was passing `gtol * 1000.0` down to
`least_squares`, so a caller asking for `1e-14` got `converged = true` at
`6.5e-12`. Harder to spot than the original, because it is inside a wrapper and
scales with the caller's own argument, so it reads as deliberate.

The pressure behind it was structural: `curve_fit` returned early on
`if (!fit.converged)`, so an accurate fit that merely missed a tight `gtol`
produced no covariance at all. Removing the multiplier alone would have
recreated the temptation. `covariance_valid` is now set from whether the normal
matrix inverts, independently of `converged` - matching SciPy, which computes
`pcov` whenever it can. The two flags answer different questions: "did I reach
the tolerance you asked for" and "is this covariance meaningful".

The probe caught the second one only after a `curve_fit` honesty assertion was
added; it had one for `minimize_bfgs` and `minimize_lbfgs` and none for
`curve_fit`. Same vacuity as the linkage and highpass cases: the check that was
missing is the one the bug lived behind.

### Documented, not changed

`curve_fit` scales the covariance by `s2 = 2 * cost / dof`, SciPy's default
`absolute_sigma=False`: `sigma` sets relative weights only and the absolute
scale is inferred so reduced chi-square is 1 by construction. Now stated at the
function, along with how to recover the unscaled covariance, and the fact that
a finite-difference Jacobian floors the gradient norm near 1e-11 so a tighter
`gtol` is unreachable without an analytic Jacobian.

## Examples

Three, all in `example/sci/`, all run by `example.bat`.

- **`tone_detection.cb`** - two tones under white noise. Welch PSD, a detection
  threshold from `Dist.chi2_ppf` rather than from eyeballing the plot,
  `find_peaks`, then a Butterworth band-pass whose noise reduction is checked
  against the brick-wall prediction. The median-bin noise floor implies a
  variance of 1.025 against a true 1.0, which independently confirms the PSD
  scaling.
- **`decay_fit.cb`** - a fluorescence decay fitted with per-point counting
  errors, reporting parameters with uncertainties and a chi-square p-value,
  then the SAME data fitted with the baseline term omitted. The wrong model
  returns respectable-looking error bars and p = 0.000000 against 0.9819; the
  pull row shows the systematic runs that produce it.
- **`pca_cluster.cb`** - six columns generated from a 2-D hidden plane. SVD
  recovers 98.9% of the variance in two directions, and the flat PC3..PC6 tail
  (spanning only 1.5x) is shown as the evidence that there is no third
  direction. k-means then recovers all three groups, compared by grouping
  rather than by label value.

`decay_fit.cb` initially shipped a false punchline: with a 5-count baseline
under noise of sd 2.6 to 9.8 the omitted term was absorbed by a longer lifetime
at almost no chi-square cost, so the p-value did not collapse and the program
contradicted itself. Fixed by making the baseline resolvable (20 counts, 60
points, twice the time range), not by softening the claim. A second false
sentence - that the wrong model's error bars were no larger - was removed after
the run showed its lifetime uncertainty was five times larger.

## Status

G1-G5 all merged and verified. Gates on this tree: `test.bat Release` all
passed 0 skipped, `test_lsp.bat Release` all passed, `example.bat Release`
100 passed 0 failed 41 skipped.

## As built: fft dependency chain

`hpc/fft.cb` needed exactly one thing from `hpc/densemat.cb` - a transpose for
the 2-D transform - and paid for it with that module's entire threading stack
(`threadpool.cb`, `hpc/parallel.cb` and their closures): 18 transitive imports.

`Vec.transpose` now lives in `hpc/vecmath.cb`, a leaf module whose only import
is `math.cb`. `Mat.transpose` forwards to it, so the public API is unchanged.

| module | transitive imports before | after |
|--------|---------------------------|-------|
| `hpc/fft.cb`    | 18 | 2 |
| `sci/signal.cb` | 19 | 3 |

Guarded by `test_import_closure()` in `Test/test_hpc_kernels.cb` - see
"Dependency-closure guard" in `internal/testing-notes.md`.
