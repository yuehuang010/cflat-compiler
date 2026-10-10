# `--symbol-dump-opt` over two files crashes (SIGSEGV) when the first file has an error

Pre-existing on master; found by the T60 review 2 (2026-10-06). `cflat a.cb b.cb --symbol-dump-opt function:main`
where a.cb has a compile error: SIGSEGV in llvm::StructuralHash via GetOrBuildOptimizedView (the
optimized-view snapshot reused across positional files - see CFLAT_VIEW_NO_INCREMENTAL in CLAUDE.md).
Repro: scratch/repro_keep/t60_rev/r2mfa.cb + r2mfb.cb. A failed file's module must not feed the next
file's incremental optimized view; add a LogError-free skip or rebuild, never a crash.
