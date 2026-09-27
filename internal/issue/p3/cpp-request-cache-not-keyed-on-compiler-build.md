# C++ request cache is not keyed on the compiler build (dev-loop hazard)

**Summary.** `CxxTypeRequestCacheKey` (`cflat/LLVMBackend_CInterop.cpp`) deliberately keys on
`kCHeaderCacheVersion`, not on the cflat build stamp, so CI reuses entries across rebuilds. In a
dev loop this means a harvest fix does NOT take effect until `x64\Release\.cflat\cheaders` is
wiped: a fixed extractor keeps replaying the stale entry (declarations, ABI and the definitions
bitcode sidecar), and a probe that "still fails" after the fix is measuring the cache.

**Repro.** Change anything in `CClangExtract.cpp` that alters a harvested record (member set,
needsLocalDefinition, emitted definitions), rebuild, rerun `Test\test_cpp_interop.cb` without
wiping the cache: the old behaviour persists.

**Root cause.** By design (see the key builder comment). The version constant is the only
invalidation and is bumped on release, not per change.

**Fix direction.** Either a dev-only key component (e.g. `CFLAT_CPP_CACHE_SALT` env var or the
exe mtime when a `--dev` flag is set), or `test.bat` wiping the cache when `cflat.exe` is newer
than the cache directory. Not a correctness issue for shipped builds; it costs developer time
(this bit twice on 2026-09-26).
