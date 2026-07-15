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

`.gitattributes` pins `*.sh` to LF so it stays runnable on a Windows checkout.
