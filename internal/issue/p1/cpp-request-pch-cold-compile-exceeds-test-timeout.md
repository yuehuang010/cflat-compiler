# C++ interop request cost: cold compile of test_cpp_interop_template exceeds test.bat's timeout

STATUS 2026-09-19: largely FIXED. `test.bat Release` is green in 243s (was 660s with
test_cpp_interop_template failing on the 600s timeout). What remains is the FIRST compile on a
machine with no `cheaders` cache at all - see "Residual" below.

## Symptom

`Test/test_cpp_interop_template.cb` compiled in ~621s from a cold C++ header cache, so `test.bat`
(TIMEOUT_SECS=600) killed it and reported "compiler error" with no diagnostic. Every buildci saw
the cold case, because the request-cache keys folded `CompilerBuildStamp()` and CI rebuilds cflat
on every run.

## Repro

    x64\Release\cflat.exe Test\test_cpp_interop_template.cb -i Test\library -o scratch\t.exe --nologo

Cold = delete `x64\Release\.cflat\cheaders` first. `-o` skips the compile when the output is newer
than its inputs ("up to date: ..."), so a timing run needs a fresh `-o` name each time.

## What actually cost the time

Two separate things, both measured on Release, serial, otherwise idle box.

**1. The cache needs TWO runs to converge, and CI never got the second.** A single prior run is
not "warm": entries land across successive runs. Per-test wall clock, cache cleared once:

| test | run 1 | run 2 | run 3 |
|------|-------|-------|-------|
| test_cpp_interop_bridge | 110s | 18s | 16s |
| test_cpp_interop | 199s | 49s | 49s |

**2. A record-only empty-harvest test made the PCH path do 8x the work.** With the PCH,
test_cpp_interop_bridge ran 289 request TUs; without it, 37. `emptyHarvest` read
`raw.records.empty()` as "the PCH gave us a broken AST", but a request whose spelling resolves to
free functions or member signatures alone - the whole std::function bridge - legitimately harvests
sigs and no records. Every such request paid a PCH TU and then a full reparse.

## The PCH is load-bearing cold and must stay on

A/B with PCH generation disabled:

| | with PCH | without PCH |
|---|---|---|
| bridge, cold | 99s | 339s |
| test_cpp_interop, cold | 304s | 563s |

Gating PCH generation behind a per-group TU counter (build only after 4 TUs) was TRIED AND
REVERTED: cold went back to no-PCH levels (test_cpp_interop 571s, bridge 274s), i.e. the threshold
is rarely crossed. A running counter is the wrong signal.

## Fixed

1. `RunCxxTypeRequests` / `RequestGeneratedCxxWrapper` no longer delete the group's shared PCH on
   suspicion. They retry first and drop it only if the retry does strictly better. Cold template
   PCH regenerations fell 73 -> 36. (Commit "Fix windows build.")
2. `emptyHarvest` also requires `raw.sigs.empty()`. test_cpp_interop steady state 49s -> 30s.
3. The type-request cache key carries `kCHeaderCacheVersion` instead of `CompilerBuildStamp()`, so
   entries survive a cflat rebuild. Measured across a real rebuild: bridge 110s -> 19s,
   test_cpp_interop 199s -> 31s. The PCH key still folds the build stamp, since a PCH belongs to
   the clang that wrote it.

`test.bat Release` after: 243s, green, test_cpp_interop_template 208s.

**Discipline this buys:** `kCHeaderCacheVersion` (LLVMBackend.h) is now the ONLY compatibility
guard on the type-request cache. Bump it in the same change as any edit to what the extractor
harvests or how an entry is read back - the same rule the numbered version history above
`TryLoadCFileSigCache` already documents.

## Cache payload size (v77)

The `cheaders` JSON had grown to 2.7 GB of a 4.4 GB cache. Three causes, all now fixed; the
schema version went 76 -> 77 in the same change.

1. **Every entry was a full snapshot.** `WriteCHeaderDiskCache` serialized the whole
   `CFileSigCacheEntry` per request, and every request against a group re-parses that group's whole
   include prologue - so all 1443 entries carried the same ~4400 signatures. Measured over 5
   entries: union 4477, intersection 4339, i.e. 97% identical. They now share a **baseline**: one
   content-addressed `sigbase.<id>.json` per header group, with each entry storing indices into it
   and only the signatures it has beyond it. A baseline is immutable, so an index can only resolve
   to what it was written against; an unloadable baseline is a cache miss, never a wrong signature.
2. **The source path was repeated verbatim** on every entry - 18.6% of signature bytes over 94
   distinct paths. Paths are now interned into a per-document `files` table (`fi`). A baseline
   seeds the table with its own paths, which is what lets an unchanged signature serialize to the
   same bytes in both and so match by index.
3. **Defaulted fields were written out.** `pnames` duplicated `ps[].n` on 5622 of 5905 signatures
   (now the `pnq` marker), `va:false` on 5894 of 5905, and an all-empty positional `defaults`
   vector spelled `[{"k":"","v":""}]` per parameter (now `nd`, which keeps the length the
   `defaultArgs.size() == params.size()` callers depend on).

Measured on a wiped cache running the three interop tests twice:

| | v76 | v77 |
|---|---|---|
| entry JSON, total | 2,705.9 MB / 1443 files | **74.7 MB / 1440 files** |
| entry JSON, mean | 1.74 MB | **0.037 MB** |
| baselines | - | 16 files, ~20 MB |
| `CHeaderJsonConvert`, warm compile | 5,614 ms | **1,841 ms** |
| test_cpp_interop_template, in suite | 208.08s | **144.39s** |

`test.bat Release` 235s green, 0 skipped. Steady state across a real rebuild: interop 37s, bridge
21s, template 131s.

## Residual

A machine with no `cheaders` at all still pays run 1: template ~726s, over the 600s timeout. CI
that persists `%USERPROFILE%\.cflat` or the per-exe `.cflat` across runs never sees this; a fresh
clone does. Options, in order:

1. Cache the requests currently recorded as "missing entry" (51 of 1016 on a warm template
   compile), including negative results - dependent spellings such as
   `std::chrono::duration<type-parameter-0-0, ...>` and generated `__cflat_dflt_*` helpers re-run
   clang on every compile because nothing is ever stored for them.
2. Find why the cache takes two runs to converge rather than one. Stage 2 / definition entries
   appear to land only after a later run.
3. Batch the member-signature wave (435 requests / 91s on a cold template compile) into one
   frontend per class; the group-level batched path in `ProcessCxxRequests` already knows the
   pending count.
4. Raise request parallelism - about 1.8x on a 10-core box (550s of request work in 304s wall).
5. Nothing evicts `cheaders`. Entries now survive a cflat rebuild by design, and the `.bc`
   sidecars (326 MB) and PCHs are what is left of the footprint. A size cap or an age sweep would
   need to keep a baseline alive while any entry still indexes into it.
