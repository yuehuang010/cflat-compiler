# doc/CLI.md: `--run` is documented as rejecting threads and `program`; on macOS it runs them

## Summary

`doc/CLI.md` (~line 135) and `CLAUDE.md` state, unconditionally:

> *Single-threaded only.* A program that spawns a thread (via the `program` construct or
> `thread<T>`) is rejected - in-process JIT'd workers would need Windows SEH unwind tables the
> JIT cannot register. Compile to an exe instead.

The stated rationale is Windows-specific (SEH unwind tables), and on macOS arm64 `--run`
executes threaded and `program`-using code correctly. A user reading the docs will needlessly
AOT-build every threaded program.

Found 2026-09-16 while dogfooding the systems surface.

## Repro

All four of these run to completion with the correct exit code under `--run` on macOS arm64
(Release, `x64/Release/cflat`):

| File | Uses |
|------|------|
| `scratch/dogfood/sys/pool.cb` | 4 `Thread`s + two `channel<int>`s |
| `scratch/dogfood/sys/echo.cb` | `Thread` + TCP sockets on localhost |
| `scratch/dogfood/sys/child.cb` | two `program` constructs, `onStdout` handler |
| `scratch/dogfood/sys/pipe.cb` | two `program` constructs wired with `>>` |

```
x64/Release/cflat scratch/dogfood/sys/pool.cb --run -i Test/library
got=50 sum=42925      exit 0
```

## Observed

No rejection, no diagnostic, correct execution.

## Expected

Either the restriction is enforced on the platforms where it applies and the docs say
"on Windows", or the restriction is genuinely gone and the paragraph is deleted. As written the
docs describe a gate that does not exist on this host.

## Fix direction

Find the `--run` capability check (grep the diagnostic text near the JIT entry in `main.cpp` /
`ArgParser.h`) and determine whether it is `#ifdef _WIN32`-guarded. Then make `doc/CLI.md` and
the `CLAUDE.md` Running section state the platform scope. Doc-only if the gate is already
Windows-conditional.
