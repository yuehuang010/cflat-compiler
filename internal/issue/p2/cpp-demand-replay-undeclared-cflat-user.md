# C++ demand replay intermittently fails with "undeclared identifier '__cflat_user'" (Windows)

Found 2026-09-28 during the Windows cold-cache crash fix (MS-ABI vftable operator delete, fixed in
CClangExtract.cpp EmitCxxDemandCompanion). Seen twice, only against the default `x64\Release\.cflat`
cache right after the first cold `test.bat` run:
`C++ demand replay failed (use of undeclared identifier '__cflat_user'); retrying with C++ caches bypassed`
(`LLVMBackend_EmitAndLink.cpp` ~2978). Before the vftable fix the bypass retry crashed (0xC0000005);
now it succeeds, but the retry is expensive: test_cpp_interop_bridge compile ~120 s instead of ~5 s warm.
Not reproduced after clearing the cache (3 attempts). Hypothesis (unverified): parallel test.bat
workers write/replay the same cached request chunks and one replays a chunk set missing the
`__cflat_user` wrapper declaration - an ordering or partial-write problem in the demand cache.
Possibly related: p2/cpp-libs-cache-intermittent-alias-miss-after-rebuild.md (same shape: first
parallel run after a build-stamp change).
Relevance: silent 20x compile-time cliff - check this first when profiling test_libs C++ import time.
