# test.sh internals: SKIP list + warm-cache path

Moved from CLAUDE.md. The invocation (`bash test.sh Release`) lives in CLAUDE.md;
this is the rationale behind the SKIP list and the warm-cache second pass.

## What test.sh runs

`test.sh` is the `test.bat` counterpart on Linux/WSL and macOS. It compiles+runs the
platform-portable subset of `Test/*.cb` (plus the `Test/errors/*.cb` negative tests)
against the native cflat, in parallel with a per-test timeout, and prints a
PASS/FAIL/SKIP summary. Run it from inside WSL: `bash test.sh Release` (or `Debug`,
`-j N`).

## SKIP list discipline

It maintains an explicit SKIP list of Windows-only tests (WinMD, the Win32/console
suites) - these are test-content/subsystem limits, not core-library gaps.

`test_fpenv` is skipped on **Linux only**: the per-thread FP environment is
implemented on Windows (`_controlfp_s`) and macOS/arm64 (FPCR.FZ via
`fegetenv`/`fesetenv`), but is still a no-op on Linux x86 (MXCSR) - see
`internal/issue/fpenv-not-ported-to-linux.md`.

C interop is **not** skipped: `test_c_interop` binds the mathlib fixture, rebuilt from
source by `Test/cinterop/build_mathlib.sh` (the archive keeps its `.lib` name on every
platform), and `test_crt` binds the system CRT headers straight from the SDK.

Keep the list honest: `test_basic`, `test_stream`, `test_process`, `test_core`,
`test_c_interop` and `test_crt` each sat on it for one incidental reason (a Win32
extern, `os.windows.*` stdio, a hardcoded `cmd` shell, a missing fixture archive - and
`test_core` for no reason at all), hiding thousands of lines of portable coverage.
Before adding a test to the SKIP list, prove the *whole file* is Windows-bound.

## Warm-cache second pass (the `--init` serializer trap)

After the cold pass, `test.sh` runs `cflat --init` and re-runs the `Test/errors/*.cb`
negative tests against the warm bitcode cache (results tagged `.warm`). `--init` is
**not** "pure optimization" but a second code path that reconstructs compiler state
from a hand-written serializer, so a field an analysis reads that is missing from the
serializer is silently dropped on a warm cache and the `expect_error` stops firing.

**Rule:** any new field on `TypeAndValue` / `StructData` / `AnnotationValue` that the
analysis reads MUST be added to the `LLVMBackend.cpp` cache round-trip in the same
change. (This rule is important enough that a short form stays in CLAUDE.md.)

## Incremental O2 view gate

`Test/tools/incremental_o2_gate.py` compares the CLI incremental view with the
same multi-file flow using `CFLAT_VIEW_NO_INCREMENTAL=1`; the pinned gate runs
from `example.sh`, while `--corpus` adds six example programs. Run it directly
with `python3 Test/tools/incremental_o2_gate.py`. The input copies must retain
the basename `probe.cb` in different directories: changing the basename makes
the root-file guard miss and silently avoids measuring the incremental path.

## A SCOPED `expect_error { ... }` cannot catch a DEFERRED diagnostic

Some diagnostics can only be decided after the whole walk finishes - the generic-interface
"was never instantiated" check, for example, must wait until every generic instantiation has
drained, so it is emitted at the very end of `LLVMBackend::Compile` (and `Analyze`). A scoped
`expect_error("...") { ... }` block has already closed by then.

The failure mode is **actively misleading, not just a miss**: the scoped block reports

```
FAIL: expected error '<substring>' did not occur
```

and exits **before** the real diagnostic is emitted, so the actual error is suppressed and the
printed reason is the opposite of the truth ("did not occur" for an error that was about to occur).

**Rule:** a negative test for a deferred diagnostic must use the **bare file-scope form**
(`expect_error("substring");` at file scope, no braces), which stays armed to the end of the
compile. `Test/errors/err_generic_interface_vtable_launder.cb`,
`err_generic_interface_unrouted_is_source.cb` and
`err_if_const_generic_interface_undecidable_dead.cb` each carry a comment saying so.

Two further traps when pinning a deferred diagnostic that AGGREGATES several offenders into one
message:

- Pin the **role**, not the generic sentence. Two versions of the tests above passed on shared
  wording while reporting a completely different role, so they asserted a leg they never reached.
- Put the declaration and the operation under test on **separate lines**. When they share a line,
  the aggregate reports whichever record sorts first and the intended leg is not pinned.

`.gitattributes` pins `*.sh` to LF so it stays runnable on a Windows checkout.

## Catching vacuous tests: the mutation harness

Four times while building `core/sci`, a fully green suite sat on top of a broken
library, and a human reviewer had to catch each one by hand: a Butterworth
highpass checked only at Nyquist (where normalisation forces the answer), a
linkage test run at n=3 (where the bug is structurally invisible), a `curve_fit`
with no honesty assertion, and an SVD that was never executed. A green suite is
not evidence that the assertions can fail.

Mutation testing measures that directly: perturb one constant or comparison in
a deployed core library, rebuild, and re-run the tests. If the suite still
passes, nothing was checking that constant.

The script that did this was a throwaway kept in `scratch/` (gitignored) - an
investigation aid, not tracked tooling. What is durable is the findings below
and the tests they produced. A library is often covered by more than one test
file, so a mutant only counts as a survivor if NO test in the set catches it;
measuring against a single file understates coverage and invents false gaps.

### If you rebuild this, it needs three guards

Each was added after the script produced a confidently wrong number:

- **Baseline gate.** An unmutated run must pass before any mutant runs. Without
  it a broken baseline reports a perfect score - the exact failure mutation
  testing exists to detect, reproduced inside the tool.
- **Stale-exe guard.** An orphaned test process from an interrupted run keeps
  holding the output path, every link then fails instantly, and every mutant
  scores as "caught". This is what produced a bogus `signal.cb` 100%; the real
  number was 35.3%.
- **Sweep recovery.** A hard kill skips the restore, leaving a mutant in the
  deployed core. Recovery must sweep the WHOLE deployed tree, not just the
  library being mutated - a stale mutant in any other file poisons the baseline.

After any interrupted run, confirm the tree is clean before trusting a suite:

```bash
diff -rq cflat/core x64/Release/core
```

### Reading the score

The score is a lead, not a metric. Classify survivors before acting:

- **Real gaps** - an entire branch or feature with no coverage. `signal.cb` had
  no test for `butter_bandpass`, and never called `kaiser` at all, so the I0
  asymptotic branch was dead code under test. These are worth fixing.
- **Measure-zero boundary swaps** - `<=` to `<` on a degenerate-input guard only
  differs exactly at the boundary. Cheap to pin, low severity.
- **Path-not-outcome constants** - iteration caps, convergence tolerances and
  Wolfe/damping constants change how an iterative solver gets there, not where
  it lands. `optimize.cb` scores ~10% largely for this reason, and pinning them
  would make the tests brittle. Do not chase this number.
- **Below the code's own error floor** - perturbing the higher-order terms of
  `signal.cb`'s I0 asymptotic expansion moves the result less than the series
  truncation error. Not pinnable, and not worth pinning.

Adding five targeted tests (blackman/bartlett value checks, a Kaiser I0
branch-continuity check, and bandpass -3 dB band-edge checks) took `signal.cb`
from 35.3% to 59.0%. The remaining survivors are predominantly the last three
categories.

### Dependency-closure guard

`Test/test_hpc_kernels.cb` walks `import` statements from `hpc/fft.cb` and
`sci/signal.cb` and asserts the closure pulls in no threading stack. The walk
returns false if any file is unreadable, so a mis-resolved path cannot look like
a pass. Proven to fail: reverting the transpose split takes fft's closure from
2 modules to 19.

## Uninitialized `new[]` inputs read as flaky numerics

`test_hpc_kernels` failed roughly a quarter of the time with `least squares p0`
at ~1e260 or `-nan`. The library was fine. The test allocated the solver's
INITIAL GUESS as `double[] p = new double[2];` and never wrote it - and
`new[]` does not zero. Usually the heap hands back fresh zeroed pages and
Levenberg-Marquardt converges; when it hands back dirty memory the solve
diverges. Same bug in the `curve_fit` guess `cp`.

Two things make this class hard to see:

- **Re-running the same exe does not reproduce it.** 25 consecutive runs of one
  binary passed; the condition depends on process heap state, so the
  reproduction is a **recompile-and-run cycle**, not a re-run. It went 2/8
  before the fix and 0/12 after.
- It reads as a numeric-library bug, so the instinct is to go debug the solver.

Any buffer passed to a routine that reads it before writing it - an initial
guess, an accumulator, a `beta`-scaled output - must be explicitly set in the
test. Scratch buffers the callee fills first are fine.
