# Perf: every LSP analysis re-compiles the core library instead of using the bitcode cache

Reported by the maintainer 2026-08-16. Verified on current master.

## Summary

The CLI compile path loads pre-compiled core bitcode from the `--init` /
`--init-local` cache (`LoadCoreBitcodeIfFresh`, ~44% faster cold start). The LSP
analysis path never even attempts it: `LLVMBackend::Analyze`
(`cflat/LLVMBackend.cpp:2278`) unconditionally calls
`CompileImportedFile(runtime.cb)`, so EVERY re-analysis (every keystroke after
debounce) re-walks and re-emits LLVM IR for the core library on that backend
slot. Only the ANTLR parse of core files is cached (`parseTreeCache_` survives
`ResetForReanalysis`; core-only by design) - the listener walk and IR emission
are not, and they dominate.

## Root cause (confirmed)

`Compile()` (`cflat/LLVMBackend.cpp:498-520`) gates on
`LoadCoreBitcodeIfFresh(bcCacheDir, platformOption)` and skips the runtime
import on a hit. `Analyze()` has no equivalent: it goes straight to
`CompileImportedFile` for `core/runtime.cb` (and transitively whatever core
imports the user file pulls in). Per-analysis cost scales with core size and is
paid on every slot of the analysis pool independently.

## Fix direction

Let `Analyze` try the same core bitcode cache as `Compile`. Two things need
checking before it is safe:

- `LoadCoreBitcodeIfFresh` replaces context/module/builder wholesale; `Analyze`
  runs after `ResetForReanalysis` on sticky pooled backends, which also rebuilds
  those - the sequencing must not leak module-bound state (same crash class as
  the `ResetForReanalysis` comments in `LLVMBackend.cpp:2444`).
- The LSP needs SYMBOLS from core (hover/completion for core types comes from
  the symbol sink during the core walk - e.g. `list__string` members). A bitcode
  load skips the walk, so core symbol registration must come from somewhere
  else: either serialize/replay core symbol-sink events alongside the bitcode
  (see the `--init` serializer rule in `internal/testing-notes.md` - hand-written
  round-trip, easy to silently drop fields), or keep a per-process core symbol
  index built once and merged into each analysis index.

The second point is the real work; without it the fix trades startup time for
broken core-type completion/hover. Measure with the repro harness in
`scratch/lsp_stale_completion_repro.py` (time didOpen -> publishDiagnostics)
before/after.
