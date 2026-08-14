# `process.cb` still names backends directly (the one os.cb abstraction exception)

Carried over from the now-deleted os-abstraction plan (the refactor itself landed
and the `os.*` surface is documented in `doc/LANGUAGE.md` under the Standard Library).

## Summary

The os.cb refactor established one rule: only `os.cb` names `os.windows` / `os.posix`
backends; everything above it calls the stateless `os.*` free functions. `process.cb` is
the one acknowledged exception and was deliberately not converted.

Its Windows path (`CreateProcessA` plus handle plumbing) and its POSIX path
(`fork`/`exec`/`dup2`/`waitpid`) are structurally divergent rather than a thin call-swap,
and it declares its own Win32 externs bound to local structs (`_StartupInfoA`,
`_ProcessInfo`). It keeps naming `os.windows` / `os.posix` directly.

`stream.cb` and `barrier.cb` need no edits - they use only the signature-stable `cv_*`
shims plus the public mutex/semaphore API.

## Fix direction

Design an `os.proc_*` surface. This is a design task, not a mechanical conversion: the two
platforms' process-spawn models differ enough that the abstraction has to be chosen rather
than derived. Until then the exception stands and is not a bug.

## Verification debt

The os.cb refactor was validated green on Darwin. `test.bat Release` on Windows was never
run against it. Anyone touching `process.cb`, `terminal.cb`, or the `os.*` backends should
run the Windows suite, since the Windows console branches of `terminal.cb` are likewise
still unconverted.
