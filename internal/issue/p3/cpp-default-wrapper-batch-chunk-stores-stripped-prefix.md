# C++ header default-wrapper batch chunk stores a stripped prefix (latent replay failure)

Found 2026-09-28 in the G1 cache-atomicity review (finding 6). No repro yet.

**Summary.** The header default-wrapper batch chunk (`cflat/LLVMBackend_CInterop.cpp` ~4498) still
stores `UnseenPrefixSource(wrappers)` without a `prefixOffset`. That is the D3 shape G1 fixed for demand
chunks: a chunk keeps only the prefix its own process had not seen yet, so a replay in another process
whose group saw a different history fails with "use of undeclared identifier '__cflat_user'" (or similar)
and forces a whole-compile bypass retry.

**Trigger.** Only when a group has seen wrapper units before its header harvest; not reproduced.

**Fix direction.** Store the whole prefix plus its offset, as `CxxIncrementalGroup::ParseRequest` does
after G1, and replay through `UnseenPrefixSource` at that offset. Pin with a check in the
`cxx_request_cache_consistency` test.sh stage.
