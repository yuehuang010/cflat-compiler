# LSP mode ignores -ftime-trace; no way to profile LSP analyses

Reported by the maintainer 2026-08-16. Verified on current master.

## Summary

`-ftime-trace` works only for the normal compile path. `cflat lsp` branches away
before any profiler wiring, and the LSP arg loop does not recognize the flag, so
`cflat lsp -ftime-trace` silently does nothing. There is no way to get a Chrome
trace of what LSP analyses spend time on (relevant for diagnosing editor latency,
e.g. the core-library re-walk in
[[lsp-reanalysis-recompiles-core-from-source]]).

## Root cause (confirmed)

- `main.cpp:50`: `argv[1] == "lsp"` returns into `RunLspServer` before the
  ArgParser setup and the `-ftime-trace` wiring at `main.cpp:367`.
- `RunLspServer` (`cflat/LspServer.cpp:1812`) hand-parses only `-v`/`--verbose`,
  `-i`/`--import-dir`, and `--lsp-pool-size`; unknown args are ignored.
- The `TimeTraceScope` annotations inside the backend (e.g. "RuntimeImport" in
  `LLVMBackend.cpp`) would feed a profiler if one were initialized, but
  `llvm::timeTraceProfilerInitialize` is never called on the LSP path.

## Fix direction

Design needed beyond just accepting the flag: the CLI writes one trace at exit,
but an LSP session runs many analyses on pooled worker threads.
`timeTraceProfilerInitialize` is per-thread (LLVM supports
`timeTraceProfilerInitialize` + `timeTraceAsyncProfilerBegin`-style use per
thread); a reasonable shape is `cflat lsp -ftime-trace` writing one
`<n>.time-trace.json` per analysis (or appending per-analysis traces to a
directory) with the doc name in the trace metadata, flushed after each
`RunAnalysisOnSlot`. Keep stdout clean - the JSON-RPC channel owns it; traces go
to files only, path announced on stderr under -v.
