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

## Update 2026-09-15 (Windows measurements, after part 2 landed)

Windows Release, this host. buildci total 400 s, of which test.bat is 203 s and
test_cpp_interop alone is 168 s in-suite (next slowest: the four err groups at 45-54 s each,
everything else under 16 s). Standalone WARM compile of the fixture is 100 s here, against the
46 s measured on macOS - so part 2 is not paying off on this host.

Measured with `-v` on a warm cache (counts per compile):

| stage                        | count | total   |
|------------------------------|-------|---------|
| clang parse stage 2          | 267   | 63.6 s  |
| clang precompile header      | 43-66 | 23-39 s |
| companion emission           | 269   | 20.0 s  |
| sig harvest                  | 271   | 12.6 s  |
| member-signature requests    | 253   | 12.0 s  |

Type-request cache on that warm run: 473 HIT, 203 MISS, every miss "missing entry". The misses
are stable run to run, and instrumenting StoreCxxTypeRequestCache shows why nothing fills them -
484 stores happen, 178 are refused at the disk step:

- 156 `allowDisk=false`. Every store site passes `!tentative && raw.firstError.empty()`, and these
  are the TENTATIVE requests (the batch pre-pass at the `!last || retryable` and
  `!incompleteTypes.empty()` call sites). The request succeeds and its result is used, but the
  tentative flag keeps it out of the disk cache forever, so it re-parses on every compile.
- 22 `group has no cache clause`: the synthesized `__cflat_user::*` group for CFlat-defined
  `[cpp] struct`s (all of section M73). That group has no import statement to carry `cache`.

Second lever, PCH churn: `PruneCxxRequestPchDir` keeps only the 8 newest .pch and deletes anything
older than 1 hour. The fixture needs ~44, so a run more than an hour after the last one rebuilds
most of them at ~590 ms each (13 rewritten in a measured run). For a suite that runs a few times a
day, that hour cutoff means the PCH cache is almost always cold.

## Fix direction (Windows numbers)

1. Make tentative requests cacheable once they are known good - e.g. store on the final
   non-tentative resolution of the same request key rather than refusing at the tentative
   attempt. Biggest single lever: 156 of 203 misses.
2. Give the synthesized `__cflat_user` group the disk cache (it has no header to stale against;
   key it on the generated source instead).
3. Raise or drop `kMaxAge` in PruneCxxRequestPchDir, and size the keep count to the group count
   rather than a flat 8. Disk is 600 MB for this fixture's PCHs - a size budget would be a truer
   limit than an hour.

All three are maintainer calls (staleness policy + cache size), so nothing here is implemented.
