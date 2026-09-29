# C++ import compile time: within 10% of clang++, cold AND warm (p1, maintainer target 2026-09-27)

**Target (maintainer, 2026-09-27).** A CFlat program that imports a C++ header must compile within
10% of `clang++ -std=c++20` compiling the equivalent C++ program (compile + link), both with an empty
cache (cold) and with a populated cache (warm). Bumped from p3 (was "The three C++ interop fixtures
gate CI wall time"); the fixture/CI history below still applies.

**Benchmark (2026-09-27, master 9163cbb7, macOS arm64 Release).** Same program both ways: libtorch
`nn::Linear(2,1)` + SGD, 200 steps (C++: scratch/cmp/train.cpp, CFlat: scratch/cmp/train.cb, flags in
scratch/cmp/flags.txt; script scratch/cmp/parity.sh). Measured under load 7-21 (buildci running):

| | time | vs clang++ |
|---|---|---|
| clang++ compile + link | 4.2 s | 1.0x |
| cflat warm | 17.6 s | 4.2x |
| cflat cold (empty CFLAT_CACHE_DIR) | 98 s | 23x |

`--check` warm = 18.6 s, so none of it is codegen or link. Warm `-ftime-trace` (19.4 s span):
LinkCxxCompanion 10.0 s (60 companion bitcode modules, CxxCompanionBitcodeParse 5.9 s of it),
CHeaderRegister / CxxTypeRequestReplay 7.3-7.8 s (55 replayed requests), CxxRequestRegisterRecords
3.1 s. Cold (spike 2026-09-27, 94 s): the header IS parsed once (CxxIncrementalGroup keeps one Sema); the
111 "clang parse" lines are 1 macro pass + 1 full parse (4.3 s) + 109 incremental chunks. The time is
after the parse: record registration 51.5 s (eager std::function member requests the program never
uses), stage-2 per-request codegen re-emitting the group's whole free-function surface (~950 ms each),
61 companions / 410 MB bitcode linked (8.9 s).

**Idle-ish baseline (load 4.4, parity.sh N=1, 14:5x):** clang++ 3.67 s | warm 16.26 s (4.4x) | cold 86.12 s (23.5x).

**Windows baseline 2026-09-28 18:14 (master 1f4090f7, FIRST Windows measurement; MSVC STL headers, MS ABI,
lld-link; Ryzen AI 9 365, idle).** Harness `test_libs_parity.ps1` (C++ twins `test_libs/<lib>/<case>.cpp` next to
the RUN-mode tier-1 cases, csv in out/parity/). Method: median of 3, fresh process pinned to affinity 0x55 (Zen5
CPUs 0/2/4/6), inside vcvars64; clang++ = `-std=c++20 -O0 -fuse-ld=lld` (cflat `-o` default is -O0) with the CRT each
lib needs (json/fmt /MD, simdjson.lib is /MT); cold = fresh empty `CFLAT_CACHE_DIR` + untimed `cflat --init` (20 files:
core bitcode + linker paths, no `cheaders/`), warm = same cache, different `-o`.

| case | clang++ | cold | cold/clang++ | warm | warm/clang++ |
|---|---|---|---|---|---|
| fmt_01_runtime | 0.69 | 0.90 | 1.31 | 0.18 | 0.26 |
| fmt_02_consteval | 0.67 | 0.89 | 1.34 | 0.16 | 0.25 |
| fmt_03_memory_buffer | 0.68 | 0.82 | 1.21 | 0.17 | 0.25 |
| fmt_04_string_arg | 0.69 | 0.89 | 1.29 | 0.17 | 0.25 |
| json_01_read | 2.06 | 4.74 | 2.30 | 1.29 | 0.62 |
| json_02_type_checks | 1.94 | 3.66 | 1.89 | 0.42 | 0.21 |
| json_03_build | 1.88 | 3.30 | 1.76 | 0.31 | 0.16 |
| simdjson_01_dom | 1.30 | 1.65 | 1.27 | 0.35 | 0.27 |
| simdjson_02_ondemand | 1.39 | 2.37 | 1.71 | 0.74 | 0.53 |

Worst cold: json_01_read (2.30x). Its `-ftime-trace` cold run (5.18 s span; clang time-trace
scopes are inlined into the same trace): CodeGeneration 2.82 s incl, ProcessImports 1.47 s (CHeaderExtract 1.38 s,
CxxGroupHeaderParse 1.22 s), RunCxxTypeRequests 1.93 s (CxxRequestStage2 1.85 s). Top self time: CodeGeneration 807 ms,
CheckConstraintSatisfaction 803 ms (x77), InstantiateFunction 661 ms (x821),
ParseDeclarationOrFunctionDefinition 413 ms, ParseClass 282 ms, InstantiateClass 278 ms, CxxGroupHeaderParse 274 ms,
RunCxxTypeRequests 221 ms, CxxDemandFlush 193 ms, CxxDemandHand 181 ms, CxxDefinitionEmit 131 ms, CHeaderExtract 110 ms,
OptModule 109 ms. Scale: `clang++ -fsyntax-only` on the same .cpp = 1.63 s, `-c` = 1.96 s, compile+link 2.06 s.

Observations: (1) fmt (header + prebuilt dll, small instantiation set) and simdjson_01 are already 1.2-1.35x cold;
the 1.7-2.3x cases are the template-heavy ones (nlohmann json, simdjson ondemand), where clang instantiation work
(CheckConstraintSatisfaction, InstantiateFunction/Class ~2 s of the cold span) is done in cflat's type-request
stages on top of the header group parse, i.e. the same work clang++ does once inline. (2) Warm meets the target
everywhere (0.16-0.62x); json_01_read warm is the outlier (1.29 s vs 0.3-0.4 for its siblings), and its warm trace
shows CodeGeneration 820 ms of 1.26 s (companion/cache load is only ~0.15 s), so warm cost there is cflat-side
codegen of the many instantiated json members, not cache replay. (3) Windows cold is closer to the 1.1x goal than
mac was at the start (1.7-2.3x here vs 23x), but still not met for json/simdjson-ondemand.

**Windows fixes 2026-09-28 (perf only; cheaders/ tree byte-identical to a pre-change cold run, no cache version
bump).** (1) Demangler: `ParseMangledCandidates` re-parsed the same suffix once per enclosing choice when a template
arity was unknown (exponential on `pair<const string, basic_json<...>>`), uncached, per overload candidate. Now a
per-call position memo (`TypeParseMemo` in TypeMangling.cpp; state is read-only during one parse, so no
invalidation question), plus a `MangledBase` shape reject in `IsCxxSharedPtrUpcast` before it demangles.
(2) `CxxGroupHeaderHash` hashed the whole header (simdjson.h 6.4 MB) on every type-request cache load/store; now
memoized per process on (path, mtime, size), same hash value. N=3, pinned 0x55:

| case | clang++ | cold before | cold after | warm before | warm after |
|---|---|---|---|---|---|
| fmt_01_runtime | 0.70 | 0.90 | 0.92 | 0.18 | 0.17 |
| json_01_read | 2.09 | 4.85 | 4.11 (1.97x) | 1.31 | 0.56 (0.27x) |
| simdjson_02_ondemand | 1.39 | 2.36 | 1.59 (1.15x) | 0.75 | 0.29 (0.21x) |

**Windows front-end baseline 2026-09-28 19:19 (same tree as the fixes above; `test_libs_parity.ps1 -Mode Syntax`
or default `Both`).** Method: `clang++ -std=c++20 -fsyntax-only` (same -I, no codegen/link) vs `cflat <case>.cb
--check` (same --c-include/--c-lib), median of 3, pinned 0x55, inside vcvars64, cold = fresh `--init`'d cache (its
own dir, not the `-o` cold's), warm = a second `--check` on that cache. `--check` runs the whole import path (header
bind, stage-1/stage-2 type requests, explicit instantiations, ODR-use helpers, in-process codegen of the request TU)
but is a batch-mode compile: it skips the demand-companion definition rounds (`cxxDemandLinks_` off, companion
refused "batch mode"), the module optimizer/baseline passes, and object emission/linking. Quirk (the 5L-style
divergence): `--check` only READS the C++ type-request disk cache, it never writes one (the cache ends with the
header entry alone, 4.8-5.5 MB, every run logs "cache MISS (missing entry)"), so its "warm" only saves the
header parse and is not comparable to the `-o` warm. Read the check-cold column as the pure front-end cost.

| case | clang++ -fsyntax-only | --check cold | cold/clang++ | --check "warm" | warm/clang++ |
|---|---|---|---|---|---|
| fmt_01_runtime | 0.55 | 0.79 | 1.44 | 0.70 | 1.28 |
| fmt_02_consteval | 0.56 | 0.79 | 1.43 | 0.71 | 1.27 |
| fmt_03_memory_buffer | 0.55 | 0.73 | 1.33 | 0.64 | 1.16 |
| fmt_04_string_arg | 0.55 | 0.79 | 1.44 | 0.73 | 1.31 |
| json_01_read | 1.69 | 3.24 | 1.91 | 3.12 | 1.84 |
| json_02_type_checks | 1.64 | 2.99 | 1.83 | 2.91 | 1.78 |
| json_03_build | 1.63 | 3.04 | 1.86 | 2.93 | 1.79 |
| simdjson_01_dom | 1.10 | 1.24 | 1.13 | 1.07 | 0.97 |
| simdjson_02_ondemand | 1.11 | 1.40 | 1.26 | 1.22 | 1.10 |

Same run, compile+link baseline (Release; matches the table above): json_01 cold 4.06 (1.91x) warm 0.55 (0.26x),
json_02 3.64 (1.83x) / 0.41 (0.20x), json_03 3.35 (1.77x) / 0.29 (0.15x), simdjson_01 1.39 (1.06x) / 0.22 (0.16x),
simdjson_02 1.60 (1.15x) / 0.29 (0.21x), fmt_01..04 0.83-0.91 (1.23-1.34x) / 0.16-0.18 (0.24-0.25x). Reading: the
cold ratio is almost the same against either baseline (json ~1.8-1.9x), i.e. the gap is front-end work
(header group parse + type-request instantiation), not codegen/link. Cold cache after a `-o` compile:
fmt 48-56 files / 4.8-5.2 MB, json_01 154 files / 11.0 MB (134 cheaders/), json_02 90 / 9.1, json_03 106 / 8.9,
simdjson_01 48 / 5.5, simdjson_02 98 / 6.5.

**Status 2026-09-28 12:25 (master bd1def8e, perf timebox 3, 09:40 -> 15:40).** parity.sh N=3, quiet
machine: clang++ 3.26 s | cflat warm 0.46 s (0.15x, met) | cflat cold 5.26 s plain LLVM (1.6x) /
4.29 s with a PGO-built LLVM (1.3x, scratch/pgo_llvm_notes.md, install llvm-23.1.0-pgo, ruling R2 in
scratch/rulings_2026-09-28.md). Sibling programs (a different torch program after a cold one) 4.3-4.5 s
(replay 33-40 chunks, was 111-124). Landed: 37f2c948 by-value gate projects only dtor + copy/move ctors
(393 -> 111 request chunks, cold -1.05 s, warm -0.36 s), 20101f74 macro prepass folded into the chunk-0
parse (cold -0.54 s), 53ac9fef emission walks (-25 ms), bd1def8e callback-ABI by-value slice (-0.04 s,
62 fewer request files). Not landed: error-body sweep seeded by reachability (branch perf/error-sweep-reach,
~45-70 ms; root set is a syntactic closure the review could not prove complete against CodeGen's implicit
edges, and no discriminating test exists - see scratch/briefs/review_errsweep_report.md). Remaining cold budget (PGO exe, -ftime-trace): chunk-0 parse ~2.0 s (clang++ pays
it too), eager Define* passes ~0.75 s (shared cost, any one pass alone saves nothing - ruling R1 demand-only
surface; Codex lazy attempt scratch/lazy_defs_codex.patch changed 376 cache members and is NOT landable),
definition emission ~0.35 s, harvest/self ~0.7 s, default-wrapper batch ~0.4 s (ruling R3). Path to 1.1x
(3.6 s) = PGO + R1; R3 is the margin.

**Status 2026-09-28 04:00 (master 85dfb8cc, perf timebox 2026-09-27 22:00 -> 2026-09-28 07:00).**
parity.sh median of 5 (04:52): clang++ 3.32 s | cflat warm 0.55 s (0.17x, TARGET MET, ~6x faster
than clang++) | cflat cold 6.80 s (2.05x, NOT met) | warm-edit (train_relu after train, new
C++ demand) 6.0 s via demand-chunk replay (edc89a47) instead of a whole-compile cold retry. torch
tier 226 s -> 81-85 s. Landed: BF BH DI BJ BK BG CJ BL BN BQ DS UV BP NS (Queue.md LANDED rows;
report scratch/perf_timebox_report_2026-09-28.md). Cold -v budget (~6.9 s): group Interpreter parse
~2.0 s, harvest (ComputeCxxAbi: pending instantiations, implicit defs, EmitCxxDefinitions) ~1.3 s,
type-request chunks ~0.8 s (335-390 chunks), default-wrapper second parse ~0.45 s, record
registration ~0.33 s, macro prepass ~0.25 s, re-harvest ~0.15 s, CFlat + companion + link ~0.5-1 s.
Our LLVM (plain Release, no PGO/LTO) runs `clang++ -fsyntax-only train.cpp` ~20% slower than Apple
clang, and ~4.5 s of the cold budget is clang work.

**Status 2026-09-27 21:00 (commit 82370cc5, P1+P2+P3 squashed: lazy std::function member binds; no
per-request surface harvest; one demand-driven companion per import group, cache 117).** parity.sh
median of 3, load ~4: clang++ 3.24-3.53 s | cflat warm 3.0-3.3 s (0.9x, TARGET MET) | cflat cold
11.4-12.4 s (3.5x). Edit-then-warm 3.6 s (companion reused). test_libs torch tier: 447 s -> 232 s cold,
49 s warm. Cold floor left (P4/P5): chunk-0 header parse 3.9 s (clang++ pays this too), RegisterCRecords
2.3 s, harvest glue ~1.8 s, CxxAbiArrange 1.2 s; plan: batch requests (P4), lazy record projection +
overlap chunk 0 with CFlat parsing (P5). Warm gap: a demand change (new C++ symbol used) still retries
the whole compile cold; design for an in-link chunk replay is in scratch/p3_notes.md ("remaining").
Spikes: scratch/extract_spike.md (cold breakdown), scratch/warm_spike.md (pre-P3 warm breakdown).

**Order (maintainer, 2026-09-27): cold to parity first, then warm.**

**Parity budget.** Warm must do no C++ parsing, so warm parity needs the replay + companion link to
be near-free: one pre-linked companion per import group loaded as one module, and one serialized
bound-surface snapshot per group instead of N request replays. Cold plan: internal/plan/cpp-import-compile-parity.md
(P1 lazy member types -> P2 no surface re-emit -> P3 one codegen per group -> P4 batched requests ->
P5 overlap/lazy mapping; expected 94 -> 43 -> 24 -> 11 -> 8 -> <=4.6 s).

**Method (maintainer, 2026-09-27).** Build times differ vastly across samples, so work ONE sample to
parity (cold and warm) before moving to the next. Sample 1 = the torch training benchmark above. Next
samples after it lands: simdjson, fmt, then the three C++ fixtures.

**Acceptance.** scratch/cmp/parity.sh (moved into the repo as a perf gate when the plan lands) on
torch, simdjson and fmt: cflat cold <= 1.1x and warm <= 1.1x clang++, measured on an idle machine,
3 runs, median. No regression in test.sh / test_libs.

---

## History: the three C++ interop fixtures gate CI wall time (buildci warm 372 s vs 300 s target)

**Summary.** `buildci.bat` Release, warm request cache, 2026-09-26: 372 s green. test.bat 144 s
and the LSP sweep 150 s are both bounded by `test_cpp_interop_bridge.cb` / `_template.cb` /
`test_cpp_interop.cb` (108 / 118 / 80 s inside the parallel suite, 100 / 78 / 62 s in the LSP
sweep). Standalone warm, the bridge fixture compiles in 42 s; the rest is core contention.

**Profile** (`-ftime-trace`, bridge fixture, warm, self time): LinkCxxCompanion 12.7 s (290
companion bitcode modules linked into the program module with `Flags::None`),
CxxTypeRequestReplay 11.2 s (290 replays), ForwardRefScan 5.0 s, CodeGeneration 3.2 s,
RebindCxxCachedSignatures 3.0 s, CHeaderJsonConvert 2.8 s, TryLoadCxxTypeRequestCache 2.7 s,
CxxCompanionBitcodeParse 2.2 s.

**Tried and rejected.** `Linker::Flags::LinkOnlyNeeded` per companion, iterated until a pass
adds nothing: every pass re-adds the same 813 private globals (initializer thunks and literals
reached through the appending `llvm.global_ctors`), so it never converges and a second pass
duplicates static initializers. A single LinkOnlyNeeded pass is unsafe on its own because a body
linked from a later companion can need a helper an earlier companion defines.

**Landed 2026-09-26 (evening timebox).** Items 1 and 2 below: bridge fixture warm standalone
41.8 s -> 31.3 s (trace total); LinkCxxCompanion 12.7 s -> 4.9 s. Remaining profile (self time):
CxxTypeRequestReplay 9.8 s (290 replays; the scope spans everything after a cache hit - record
registration, signature rebinding, nested requests), LinkCxxCompanion 4.9 s, CodeGeneration
3.1 s, RebindCxxCachedSignatures 2.8 s. Bridge/template/interop warm: 33 / 46 / 18 s.

**Fix direction (ordered by expected payoff).**
1. DONE. Request-local promotion is lazy: `usedFunctionWork` entries are handed to CodeGen with
   `HandleTopLevelDecl` only, so a body is emitted exactly when a Phase 2 root reaches it.
2. DONE. Companions merge into one scratch module, then ONE `LinkOnlyNeeded` pass into the
   program. Note llvm::Linker drops an unreferenced linkonce definition when the destination
   does not name it - the scratch module pre-declares every linkonce definition before its blob
   (`DeclareLinkOnceDefinitions`) or later blobs' calls stay unresolved.
3. PARTLY DONE 2026-09-26 (night). Profiled: the replay cost was `RegisterCSignatures` - every
   request hands back its group's whole free-function surface (~3.3k sigs, 952k across the
   fixture). A declaration already bound with the identical retyped signature is now skipped
   (`cxxBoundSignatureKeys_` + `IsCxxDeclarationRegistered`): 8.8 s -> 4.3 s self. A one-shot
   compile no longer copies disk-hit request entries into the memory cache (~2.5 s). Bridge warm
   standalone 31 s -> 25 s; test.bat warm ~125 s -> 113 s. Remaining (self): LinkCxxCompanion
   4.9 s, CxxRequestRegisterSignatures 4.3 s, CodeGeneration 3.1 s, Rebind 2.9 s,
   CHeaderJsonConvert 2.6 s. Next: stop returning the whole group surface per request (register
   only signatures the request's own records/namespaces need), which would cut Rebind too.
4. Make the request cache carry the merged per-group companion instead of 290 sidecars.
5. `test.bat` scheduling is a non-item: every worker starts at once, so order does not matter;
   the in-suite cost is core contention.

## FIXED 2026-09-26 (night): request cache converges on the SECOND run, not the first

Root cause: `ExtractCHeaderClang` seeds the import group's namespaces (signature spellings +
included headers, e.g. `std`); a header-cache hit skipped that, so `CandidateCxxGroupsFor` ordered
groups differently and `std::shared_ptr<...>` went to another group. Header cache entries now
store the namespaces extraction seeded (`cxxGroupNamespaces`, cache v103) and both hit paths
replay them. Verified: wipe -> cold stores 678, next run stores 0, candidate order identical.
Original observation kept below.

Bridge fixture: cold run stores its entries; the next run still STORES 83 entries (4.6 s in
`StoreCxxTypeRequestCache`, 204 files rewritten under `cheaders/v102`); the third run stores
nothing. The second-run keys have no cold-run twin: the same C++ spelling (e.g.
`std::shared_ptr<__cflat_user::SharedLeaf>`) was attributed to a different OWNING import group
(`cpp_interop_sfinae.h` on the cold run, `cpp_interop_basic.h` on the warm run) with a
different request-source hash, so group attribution depends on which earlier requests came
from cache. Steady-state CI (cache persisted across runs) is unaffected; a freshly wiped
cache pays it once. Fix direction: make the owning group of a type request independent of
cache state (attribute by the header that declares the record, not by the request that first
reached it).
## LANDED 2026-09-27: LSP analysis reads the compile's |EDEF entries (read-only)

Type requests were already mode-independent (stage 2 keys EDEF in both modes). The LSP-only
cost was C++ header imports (15 cold clang parses, 8.4 s) and generated wrappers (98 clang runs,
3.5 s): both keyed EDECL, which nothing stores. LSP mode now loads the EDEF header entry and, on
an EDECL miss, the EDEF wrapper entry (positive and |NEG), drops the companion bitcode, and never
writes or deletes on disk. It binds exactly what the compiler binds, the stated design goal.
Bridge `--symbol` run 32.6 s -> 21.2 s; LSP sweep 146 s -> 129 s (fixtures 50/59/80 s ->
30/39/51 s). Needs a prior compile to populate the cache (buildci runs test.bat first); a fresh
cache falls back to parsing as before.

## Tried and rejected 2026-09-26: letting LSP analysis write its |EDECL request-cache entries

The LSP sweep (buildci's second-largest block, 144 s warm) never hits the request cache: LSP
mode looks up |EDECL keys that nobody stores, so every sweep re-harvests the three fixtures
through clang (58 / 73 / 97 s). Dropping the `symbolSink_` refusal in `StoreCxxTypeRequestCache`
and the two header-cache write gates made it WORSE: sweep 315 s on the writing run (20 pool slots
writing the same entries), 181 s on the next, and `test_cpp_interop_bridge.cb` fails from disk
with "'make_shared' is not a member of namespace 'std' (clang: use of undeclared identifier
'__cflat_user')" - a body-less LSP entry replays a generated-prefix dependency that its key does
not carry. Reverted. Any future attempt must first make the LSP request key carry the generated
prefix (or key the |EDECL entry on the same request source a compile would build) and must
throttle writes across pool slots.


## Next investigations (ranked, 2026-09-28)

Items 1-6 of the 2026-09-27 list, the test_cpp_interop move analysis (BF: 163 s -> 0.27 s) and the
in-link demand replay (BP) are done. Remaining cold levers, all needing a ruling or large work:
1. Background cache warmer (RULING): cold does demand-only work (lazy harvest, est. ~4 s = ~1.2x)
   and a detached `--warm-cache` process fills the eager harvest for siblings afterwards. Changes
   warm = 0 from "always" to "eventually". Lazy harvest alone breaks warm-0 on 10/10 sibling torch
   fixtures (scratch/lazy_harvest_spike.md).
2. PGO + ThinLTO build of the pinned LLVM (maintainer-owned bootstrap change): est. -0.7..-1.0 s
   cold, scales every clang stage.
3. PCH / ExternalASTSource-aware harvest (scratch/pch_spike.md): clang+PCH compiles train in
   1.22 s; Interpreter sees only 1 decl from a PCH today. Attacks the ~2 s group parse.
4. Small: re-harvest after completing incomplete specializations (~150 ms); C records holding C++
   records by value project eagerly at registration (~3%).
Dropped (do not retry as-is): BI lazy implicit defs (ABI/linkage drift on 2044 records), dependent
spelling request filter (no gain), wrapper direct param spellings (no gain), BR speculative
Interpreter parse of scope-open headers (hangs).
