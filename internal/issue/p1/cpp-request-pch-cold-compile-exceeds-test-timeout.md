STATUS 2026-09-22 09:40: Phase 2 (header as chunk 0, on-mode simdjson cold 2.5 s / exactly one header parse,
warm 0.7 s / zero parses) exists on branch feature/cpp-incremental-phase2 (81a0fa4b + an uncommitted default-on
+ CFLAT_CPP_MAX_HEADER_PARSES guard attempt, patch scratch/opus_wip_0923.diff) but is NOT landable: in on-mode
test_cpp_interop_template and _bridge fail, because the request ODR-use builder there reached "budget 1" by
name-matched suppressions (every non-ctor member of any record named *optional*, shared_ptr::operator[],
Payload::operator==, simdjson_result::get - all must go), constructors were dropped as "__" members (fixed in
the patch), and libc++ hidden-friend comparison operators are never harvested from a request chunk (partial fix
in the patch). Ruling stands: incremental becomes the default only with cold budget 1 / warm 0 enforced for
every test_cpp_interop* on the default path. What landed on master from that work: the post-stage-2
use-after-free on st.incompleteCxxTypes (QualTypes read after the CompilerInstance died; Debug LLVM caught it),
the unconsumed llvm::Expected on a failed incremental Parse, and a compile error for `!` on an aggregate
(was an LLVM assert in CreateNot).

MEASURED 2026-09-22 02:50, after c0cedadc (master): the persistent-Sema direction (1c)/(plan-level) is now on
master behind `CFLAT_CPP_INCREMENTAL=1`: cflat/CxxIncrementalGroup.cpp keeps ONE clang::Interpreter per import
group (chunk 0 = the include prologue, each type request = an appended Parse chunk harvested from its
PartialTranslationUnit by the same HarvestTranslationUnit the request TUs use). Default path unchanged
(cache entries byte-identical to d96eb929). simdjson s1 cold: off 5.5 s -> on 4.3-4.7 s, warm 0.6-0.7 s.
Spike numbers (scratch/interp_spike/REPORT*.md): header chunk 0 = 635 ms live, 0.07 ms from a PCH built
with -fincremental-extensions; request chunk 0.43 ms; fresh CodeGenerator per request 2.2 ms for 10 requests;
after any failed request chunk later requests stay 10/10 with a DiagnosticConsumer installed (never Undo;
recreate from PCH = 105 ms if ever needed). Also: d96eb929 completes incomplete specializations in the live
Sema (one header parse fewer). Still open in on-mode: each on-mode .bc entry has ~245 fewer weak inline
definitions than legacy (cache-wide union differs by 23 names, libc++ <compare> helpers; probe links), json
entries differ (input_line_N source names), the Interpreter is leaked at teardown (clang aborts on
destruction after the fresh CodeGenerators ran), std/smart-pointer spellings still take the legacy path, and
the header extraction + default-wrapper parse are still separate frontends (Phase 2: header as chunk 0, and
the request PCH built with -fincremental-extensions, are the remaining ~2 s).

MEASURED 2026-09-21 23:20, after 0791cb7b (fix direction (1) landed + request-prune order fix):
- libtorch t1 (`import cpp "torch/torch.h" cache;`, fresh CFLAT_CACHE_DIR): cold 259 s / 128 TUs / 82
  requests (was 35+ min, 600+ TUs); warm 14 s, 82 disk hits, 0 TUs (was 212 s: the header-entry write
  pruned the group's request entries AFTER registration had stored them; prune now runs before extraction).
- simdjson s1: cold 6-7 s, warm 1 s. Cold profile (s1_cache.time-trace.json): core imports 0.36 s; header
  parsed FOUR times without PCH at ~0.8 s each (stage-2 full parse, the incomplete-specialization retry at
  CClangExtract.cpp:4395, the default-wrapper second parse at LLVMBackend_CInterop.cpp:3668, the request
  PCH build); then 15 PCH-backed request TUs at 0.16 s (stage 1) / 0.4 s (stage 2 CodeGen) = ~3 s.
  clang++ -std=c++20 -fsyntax-only on the same header: 0.75 s (0.15 s with PCH). A full AST walk over a
  PCH-backed TU costs 0.74 s vs 0.98 s parsed (-ast-dump-all), so the PCH only pays for lazy walks.
- Remaining directions: (1b) complete incomplete specializations in the live Sema (RequireCompleteType in
  HandleTranslationUnit, then re-walk) instead of re-parsing the header; (1c) run the wrapper parse on the
  request PCH; (2)-(4) as below; structural bound = one clang frontend per type request, so C++-like cold
  cost needs a persistent Sema per import group (clang-repl style incremental completion) - plan-level.

MEASURED 2026-09-21 22:30-22:45, simdjson as the model (scratch/simdjson_spike/sj_r1.log + sj_r1.trace.json,
fresh CFLAT_CACHE_DIR, `import cpp "simdjson.h" cache;`), with a plain-clang comparison point:
- clang++ -std=c++20 on a TU including torch/torch.h: 4.2 s parse+codegen, 1.2 s with a PCH; simdjson.h
  0.4 s. cflat cold on torch t1: >600 clang TUs at ~2.2 s (PCH used) = >25 min. It is not "reading a
  header slowly", it is running hundreds of clang frontends.
- Request tree for s1 (2 source-level types -> 30 requests, 62 TUs, 18 s): root simdjson_result<dom::element>
  13.6 s. Its 10 children (simdjson_result<bool/double/long long/string_view/...>, <vector<element>>) come
  from the EAGER refused-member retry in RegisterCxxClassMembers (LLVMBackend_CInterop.cpp:11764): every
  requested specialization retries every member refused for "returns/takes unsupported type", which
  requests that type, which registers a specialization, which retries ITS members - a transitive closure
  over return/parameter types regardless of what the program calls (t1 calls torch::ones only; 331
  requests, 96 of them std::function<...> callback members). vector<element> then pulls 7 libc++ iterator
  types (3.9 s) through CollectCxxMemberRequestItems (begin/end/operator->/operator*).
- Every request is stage 1 + stage 2 = 2 clang TUs, serial on one thread.
- PrewarmCxxRequestBatch (one stage-1 + one stage-2 TU for a whole nested closure) is DISABLED when the
  group has the `cache` clause (:9049 `if (group.diskCache) return;`). So: with `cache` you get disk
  persistence but no batching; without it batching but nothing persists. Neither mode gets both.
  UPDATE 2026-09-22: the `cache` clause is removed and every import is disk-cached; the incremental
  executor (one Interpreter TU per import line) replaced request TUs, and PrewarmCxxRequestBatch is
  deleted. Direction (4) is done; (2) is moot.
- Fix directions, in order of payoff: (1) make the refused-member retry lazy (retry at first use site,
  which TryBindRefusedCxxMember already supports) instead of eager at registration; (2) allow batching
  with disk cache (write per-item entries from the batch result, as the comment at :9049 already
  proposes); (3) run request TUs in parallel; (4) default `import cpp` to disk persistence.

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
