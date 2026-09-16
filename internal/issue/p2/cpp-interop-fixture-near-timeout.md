# Test/test_cpp_interop.cb cold compile is ~117 s, at the suite's per-test timeout

## Summary

Standalone cold compile of Test/test_cpp_interop.cb (Release, arm64 macOS, 2026-09-14):
96 s at commit 99f4488c, 117 s with the M78 rows (make_shared / shared_ptr of `[cpp]`
structs) in the working tree; in-suite under parallel load 80 s -> 112 s. test.sh's
per-test timeout was 120 s, so the fixture was one busy core away from a spurious FAIL.
Stopgap applied 2026-09-14: test.sh TIMEOUT_SECS 120 -> 240 (test.bat already uses 600).

## Cause

Every C++ type request (class-template specialization, function-template wrapper, generated
`[cpp] struct` class) is a stage-1 parse plus a stage-2 parse+CodeGen against the group's
PCH, and only the header entry is on disk: the requests themselves live in the per-process
`cFileSigCache_` and re-run on every compile (part 2 of the header disk cache, HELD by the
maintainer for staleness; internal/issue/p2/cpp-header-cache-blob-budget-and-type-requests.md).
The fixture keeps growing by one section per interop feature, each adding 5-20 requests at
roughly 1-3 s apiece; the baseline compiler is not slower per request (measured: 96 s vs
94.5 s on the same rows).

## Fix direction

- Real fix: type-request disk cache part 2 (design in the held issue above).
- Interim: when the fixture passes ~150 s standalone, split the interop rows across the
  existing test files by theme (e.g. `[cpp] struct` rows into a second interop file only with
  maintainer approval - new Test/*.cb files need a ruling) so no single test holds all
  requests; or make test.sh give known-heavy tests a per-test budget instead of a global one.
- Keep the M-section request count visible: `-v` prints each request; a section adding more
  than ~10 should justify it in its header-index line.

## Update 2026-09-16

Type-request disk cache part 2 implemented (worktree cflat-cxJ, branch wip/cxJ, uncommitted) and
every `import cpp` in the fixture carries `cache`. Measured there: cold 128 s, warm 46 s, both
exit 0. The suite's cold pass still pays the cold number, so the 240 s timeout stays; the
remaining cold lever is batching requests per group in the pre-pass (see the plan discussion).
