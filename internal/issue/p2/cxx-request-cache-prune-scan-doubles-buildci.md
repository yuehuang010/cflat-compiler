# C++ request-cache prune scan doubles buildci after a build-stamp change

Summary: when the compiler's build stamp changes (any rebuild of cflat; default
`CFLAT_CACHE_BUILD_STAMP` = the build) over a full local cache, every C++ request store runs
`PruneCxxTypeRequestDiskCache` (cflat/LLVMBackend_StateAndImports.cpp ~4572), which scans the whole
`cheaders/v<N>` directory, opens every `.rq` older than 10 minutes, and does this once per
owner / validity / scope - while holding `gCHeaderDiskCachePublishMutex`, so the other compile
threads wait. The suite then runs about 2x slower, and every timing taken on the main checkout after
a rebuild is inflated. No result is wrong; this is compile time.

Measured (Mac, test.sh Release, test_cpp_interop / _template / _bridge seconds):

| Run | test.sh | interop | template | bridge |
|---|---|---|---|---|
| fresh or cleared local cache (bar runs, typical) | 500-620 s | 280-420 | 23-33 | 23-32 |
| stamp flip over a full cache (bar or buildci on main) | 880-1120 s | 670-900 | 136-203 | 86-120 |
| 2026-11-07 buildci on d91b6671 (500 MB, 76k files local cache) | 933 s | 759 | 172 | 106 |

2026-11-02 `sample`: same-stamp warm compile 34 s; first compile under a foreign stamp 170 s with
sys ~105 s. File count alone is not the cause (a cold run refills ~66k files, 33k `.rq`, 252 owner
groups, one flat directory); the cost is the per-owner full-directory scan + open under the mutex,
roughly owners x entries file opens on the first run after a stamp flip.

Code:
- Store path: `WriteCHeaderDiskCache` (LLVMBackend_StateAndImports.cpp ~4183) writes the `.json`
  entry and its `.rq` marker (line 1 owner, line 2 validity = stamp + header mtime/hash, line 3
  prune scope), then takes `gCHeaderDiskCachePublishMutex` (file-static, ~46) and calls the prune
  (~4526).
- `PruneCxxTypeRequestDiskCache` (~4572): a function-static `pruned` set dedups on
  directory + owner + validity + scope, so the scan reruns for each new owner/validity pair; it
  iterates the whole directory, skips entries younger than 10 min (`grace`), opens each remaining
  `.rq`, and removes the entries whose owner and scope match but whose validity is not current.
- Stamp: `GetBuildStampState` (~2230). `CFLAT_CACHE_BUILD_STAMP` = unset or `1` -> exe mtime+size;
  `0` -> no stamp; any other value is appended, so a new value flips the stamp without a rebuild.

Repro (no rebuild; leaves the real cache alone; script `scratch/prune_repro/run.sh`, gitignored):
1. A cache whose `.rq` entries are older than 10 min - copy a used local cache
   (`cp -R x64/Release/.cflat <dir>`), or fill one with a test.sh run and age it
   (`find <dir> -name '*.rq' -exec touch -t 202601010000 {} +`).
2. Same stamp, twice: `CFLAT_CACHE_DIR=<dir> /usr/bin/time -p x64/Release/cflat
   Test/test_cpp_interop_template.cb -i Test/library -o <out>`.
3. Flip: same command with `CFLAT_CACHE_BUILD_STAMP=probe-a` - slow, high `sys`.
4. Same new stamp again - fast (entries now rewritten under probe-a).
5. Control: an empty `CFLAT_CACHE_DIR` (after `--init`) with a new stamp - the cold time without
   the prune penalty.

Repro run 2026-11-07 (Mac, copy of the main local cache: 499 MB, 37,595 `.rq`; one compile of
test_cpp_interop_template):

| Step | real | user | sys |
|---|---|---|---|
| same stamp (first run on the copy) | 9.4 s | 7.8 | 2.1 |
| stamp flip over the full cache | 27.1 s | 21.6 | 16.6 |
| control: empty cache, new stamp (cold) | 20.8 s | 19.1 | 3.5 |

The flip is slower than a fully cold compile, with ~5x the sys time (the scan + opens). One
process alone loses ~6 s; in test.sh the interop TUs flip together and share the disk, which is
where the 2x comes from. Pitfall: give each timed run its own `-o` - the repeat runs above took
0.01 s because the output was up to date and the compile was skipped, so they measured nothing.

Stopgap in use: clear the local cache (`cflat --init-clear-local && cflat --init-local`) before a
timed run. buildci.sh / buildci.bat do not do this, so a buildci right after a rebuild hits the slow
mode.

Fix direction (pick one, measure):
- One scan per directory per process: read each `.rq` header once, index owner -> entries, prune
  from the index; later prunes for other owners use the index instead of reopening files.
- Shard the directory by owner (`cheaders/v<N>/<owner-hash>/`) so a prune lists only its own owner.
- Move pruning off the store path: prune at `--init-local`, or once at process exit, outside the
  publish mutex.

Acceptance: on the same commit, a stamp flip over a full local cache runs test.sh within ~10% of a
cleared-cache run (test_cpp_interop_template back to ~25-35 s on the Mac); header-parse budgets
(cold = 1, warm = 0) and test_cpp_interop's cold/warm parse counts unchanged; torch tier cold and
warm unchanged; dead entries are still removed (cache size after repeated stamp flips stays bounded).

## Status 2026-11-07 (branch fix/prune, uncommitted) - stays OPEN, narrowed

Done: `PruneCxxTypeRequestDiskCache` reads the request markers once per process and cache
directory (index owner -> marker, cflat/LLVMBackend_StateAndImports.cpp `CxxRequestPruneIndex`,
built outside the publish mutex). Dead/live rule unchanged. Dead-entry removal equals master
(same test: 3427 stale entries removed, 3203 written). Single-process flip, test_cpp_interop_template:
sys 16.5 s (master) -> 5.3 s, control 2.8 s; wall 27.8 s -> 26.9 s, control 21.1-21.5 s.

Still open:
- One 35k-file scan per storing process remains: flip wall is still ~25% over cold per compile.
- Real flip (touch the exe; .rq aged past the grace) over a full cache, test.sh on the Mac, same
  build, 2026-11-07: test_cpp_interop / _bridge / _template 269 / 26 / 27 s vs cleared-cache
  279 / 25 / 25 s (master flip: 759 / 106 / 172). The request-cache part is fixed.
- Fixed 2026-11-07: the rest of the flip suite gap was test.sh's first err pass running on
  stale core bitcode (20 err files 11.6 s stale vs 1.0 s warm), not this path. test.sh now runs
  `--init-local-prune` up front, as test.bat already did.
- Windows 27 GB working set: NOT this path. A cold compile with an empty cache peaks the same
  (27.3 GB Windows, 22.9 GB Mac); moved to p2/cold-cpp-interop-compile-peaks-23-27-gb.
- Stamp flips do not grow the cache by a whole cache's worth in this path (one template
  compile under a new stamp added 1 .rq); header-content changes prune stale owners as above.
