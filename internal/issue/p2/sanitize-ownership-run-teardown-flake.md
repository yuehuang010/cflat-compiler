# `--sanitize=ownership --run` hangs or segfaults nondeterministically after main returns

Filed 2026-09-03 while landing the init_capacity poison fill (found because a probe pipeline
sat 35 minutes on a process that had already printed every line of its output).

## Summary

A program that compiles and runs clean as an AOT exe (with or without `--sanitize=ownership`)
and clean under plain `--run` intermittently fails to exit under `--sanitize=ownership --run`.
Program output is complete every time; the failure is in teardown after `main` returns.
Observed outcomes on master 5b4947e, macOS arm64 Release, 12-run samples:

| mode | result |
|------|--------|
| `--run` | 12/12 exit 0 |
| `--sanitize=ownership -o exe`, run exe | clean |
| `--sanitize=ownership --run` | 10/12 exit 0, 2/12 hang (killed by timeout); other samples also gave SIGSEGV (139) and exit 1 |

Predates the poison fill: the same rates reproduce with the master exe and with `array<T>.init`
(zero memset) instead of `init_capacity`. Single-array programs flake less often but still do
(u8 and u16 one-array probes each failed 1/3).

## Repro

`scratch/q03u_rev1_types.cb` (ten `array<T>` locals over i8..u64/float/double, each
`init_capacity(4)` then one element read). Loop it:

    for i in $(seq 1 12); do timeout 15 x64/Release/cflat probe.cb -i Test/library --sanitize=ownership --run >/dev/null 2>&1; echo -n " $?"; done

Expect a mix of 0, 124 and 139. Does not reproduce under lldb in the one attempt made (timing).

## Root cause

Not found. Shape says: sanitizer runtime state (the ownership shadow / HeapAudit-style
allocation tracking) torn down in the in-process JIT after the program's globals and locals
have been freed, racing or double-freeing with the host's own exit path. AOT never sees this
because process exit reclaims everything. Not the HeapAudit module-resolution gap ruled no-fix
2026-09-03; that one fails deterministically at compile time.

## Fix direction

Reproduce under a Debug (assertions-on LLVM) build with `MallocScribble`/guard malloc to turn the
timing flake into a deterministic abort, then decide whether `--run` should skip the sanitizer's
exit-time reporting or run it before the JIT is destroyed.
