# C++ incremental interpreter is leaked on teardown; batch `--check` grows per file

## Summary

`~CxxIncrementalGroup` (`cflat/CxxIncrementalGroup.cpp`) calls `impl_->interpreter.release()`
instead of destroying the `clang::Interpreter`, per its comment "Clang's interpreter teardown is
unsafe after CodeGen has visited its live AST". The interpreter owns the whole group's AST, Sema,
preprocessor, file buffers and LLVM modules, so all of it stays resident until process exit.

`LLVMBackend::cxxIncrementalGroups_` (one entry per C++ import group) is cleared before object
emission and in `ResetForReanalysis`. A single-file compile leaks at most one interpreter per
import group, reclaimed at exit - harmless. Batch `--check a.cb b.cb ...` runs
`ResetForReanalysis` between files, so every file orphans its interpreters and memory grows
linearly with the number of C++-importing files: ~1.3 GB peak for a 25-file `test_err.bat` group.

Not related to import ownership: the map is backend-owned, keyed by headers + defines, no refcount.

Exposed when batch mode stopped skipping incremental requests (the `!batchMode_` gates were
removed so batch `--check` meets the `--error-on-cpp-reparse cold` budget). The release itself
predates that (0911b337 / 75617db5).

## Repro

`test_err.bat` normal groups, or directly:

```
x64\Release\cflat.exe --check -i Test\library Test\errors\err_cpp_*.cb
```

Watch peak working set climb per file (Task Manager / `Get-Process cflat`).

## Root cause (not yet confirmed)

The teardown crash that motivated `release()` is unverified. Suspect destruction order: cflat's
CodeGen (`EmitCxxDefinitions` in `CClangExtract.cpp`) or a consumer in the interpreter chain
(`ChunkConsumer` announcer) still references the AST / ASTContext while the interpreter tears it
down, or a module handed out via `headerModule` outlives its LLVMContext.

## Fix direction

1. Replace `release()` with a normal destroy, reproduce the crash under a Debug (assertions-on)
   build, and fix the ordering so teardown is safe. Removes the leak everywhere.
2. Do NOT reuse interpreters across batch files: each file appends its own user-record
   declarations and retry renames, so state would bleed between files.
3. Do NOT cap batch mode back to legacy full parses: that breaks the reparse budget.

Acceptance: batch `--check` peak memory stays flat per file; `test.bat` Release green with
`--error-on-cpp-reparse` unchanged.
