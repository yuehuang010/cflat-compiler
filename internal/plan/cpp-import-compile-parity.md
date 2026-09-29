# Plan: C++ import cold compile at clang++ parity (torch training sample first)

Spike 2026-09-27, master 9163cbb7, macOS arm64 Release. Measure + design only; no source changed.

**Sample (the only target until it is at parity):** scratch/cmp/train.cb vs scratch/cmp/train.cpp
(libtorch nn::Linear + SGD, 200 steps). clang++ -O0 compile+link = 4.2 s idle (4.55 s measured
during this spike at load ~15). Parity bar = cold <= 1.1x = **4.6 s**. Next samples (simdjson, fmt,
json, eigen) are a later stage only (section 5, P6).

Artifacts: scratch/cold_spike/ (cold.time-trace.json, v.txt = cold -v, err.txt, train_cpp.json =
clang++ -ftime-trace, tree.py / win.py = trace aggregators). One cold torch compile was run
(94.2 s wall, load 15.7 -> 8.1); the earlier 98 s run (scratch/cmp/cold_out.txt) agrees.

---

## 0. Headline correction

The p1 issue says "111 clang parses (one TU per type request, serial)". That is not what happens.
The default path (CFLAT_CPP_INCREMENTAL unset) already keeps ONE live clang Sema per import group:
`CxxIncrementalGroup` wraps a `clang::Interpreter` with a no-op `ParseOnlyExecutor`. The 111
"clang parse" lines are: 1 preprocess-only macro prepass + **1 full header parse (incremental
chunk 0)** + 109 incremental chunks (2 per type request) on that same Sema. No PCH is built or
loaded on this path (0 "clang precompile header" lines).

The time goes into what each chunk does AFTER parsing: a fresh clang CodeGenerator per request that
re-emits the group's whole bound free-function surface (~13k definitions, ~6.8 MB bitcode per
request), then serializing, caching, re-parsing and linking those 61 modules (410 MB). And 42 of the
54 requests are std::function specializations the program never names, pulled in eagerly at record
registration. The parse itself is ~4.3 s, i.e. already at the clang++ floor.

So "one clang parse per import group" is done. Parity needs: (a) no eager requests, (b) no per-request
CodeGen, (c) one demand-driven companion per group, (d) batched request chunks, (e) the header parse
overlapped with CFlat front-end work.

---

## 1. Measured breakdown of the cold compile (94 s)

Timeline from `-ftime-trace` (cold.time-trace.json). Stage timers come from `-v`.

| t (s) | Phase | Wall | What |
|---|---|---|---|
| 0.0 | RuntimeImport | 0.4 | core runtime.cb (cold-cache floor for hello-world: 0.45 s) |
| 0.4 | CHeaderExtract, before registration | 17.5 | see A |
| 17.8 | RegisterCRecords (header records) | **51.5** | 42 eager type requests, see B |
| 70.1 | CodeGeneration train.cb | 14.5 | 12 program-driven type requests, see C |
| 84.6 | LinkCxxCompanion | 8.9 | 61 companion modules, 410 MB bitcode: parse 5.0 + link 3.7 |
| 93.5 | emit + ld64 | 0.4 | |

**A. Header extraction (17.5 s)**
- Macro prepass `ExtractCxxMacroPrepass` (preprocess only, own CompilerInstance): 0.68 s.
- Chunk 0 = `CxxIncrementalGroup::Create` -> `Interpreter::Parse(headerSource)`: ~4.3 s parse + 0.99 s
  PerformPendingInstantiations. This is the ONLY full header parse. clang++ spends 3.6 s in
  Source + 0.86 s in PerformPendingInstantiations on the same header (train_cpp.json), so the
  parse is at parity already.
- Harvest + in-place completion of 113 incomplete specializations + re-harvest: 0.4 s.
- Header companion `ComputeCxxAbi` (CxxAbiArrange/CxxDefinitionEmit): 3.74 s, emitting
  **88,874 definitions / 40 MB** of bitcode, + 0.38 s serialization.
- Default-argument wrappers: generation 0.01 s + "default-wrapper second parse" chunk 3.66 s
  (includes 2.1 s of clang's own Backend/Optimizer pipeline run by the Interpreter on that PTU,
  and a 0.59 s companion emit).
- Remaining ~3 s: CHeaderExtract self time (JSON conversion, record pruning/mapping).

**B. Record registration (51.5 s) - pure waste for this program**
`RegisterCRecords` -> `RegisterCxxClassMembers` -> `mapStdFunction` (LLVMBackend_CInterop.cpp
~:13978) -> `StdFunctionSpecializationForSpelling` -> `RequestCxxType("std.function", ...)`.
Every header class member whose parameter or return is a `std::function<R(Args)>` requests that
specialization at registration time, whether or not the program touches the member. 21 distinct
std::function types -> 42 requests (std::function + its __value_func base).
Self-time split: CxxDefinitionEmit 30.9 s, RunCxxTypeRequests 7.2 s (chunk parse + Interpreter's own
per-PTU CodeGen), StoreCxxTypeRequestCache 5.3 s, CxxTypeRequest 1.8 s, RegisterCRecords 3.1 s.

**C. Program-driven requests (14.5 s)**
12 requests: c10::ArrayRef<long long> (+HeaderOnlyArrayRef base), std::shared_ptr<LinearImpl>,
torch::nn::Linear, std::optional<bool>, std::optional<c10::ArrayRef<at::Tensor>>,
c10::ArrayRef<at::Tensor> (+base), and 2 more std::function pairs reached through
`ArrayRef::allMatch(const std::function<...>&)` (CxxRequestMemberTypes). CxxDefinitionEmit 9.1 s.

**Per-request cost (both B and C)** - each request is two chunks on the live Sema:
- Stage 1 (`CxxRequestStage1`, emitDefinitions=false): ~70 ms (median 72 ms).
- Stage 2 (`CxxRequestStage2`, emitDefinitions=true): ~950 ms, of which ~740 ms is
  `ComputeCxxAbi` -> `clang::CreateLLVMCodeGen` (a NEW CodeGenerator + LLVMContext per request) +
  `EmitCxxDefinitions`, ~80 ms bitcode serialization, then ~60 ms cache store.
- Why stage 2 is so big: one dumped request module (c10::ArrayRef<long long>, 6.8 MB) holds 13,068
  definitions, 5,874 of which are unreferenced linkonce_odr roots - 3,056 in `at::`, 419 `c10::`,
  376 `std::`. The request's own members are a few dozen. `ExtractCxxIncremental` harvests the
  shared header root for every request (`HarvestTranslationUnit(headerRoot, ...)`), so `abiWork`
  holds the group's whole free-function surface and `EmitCxxDefinitions` hands all of it to CodeGen
  again. 55 of 61 companions are 6.8 MB. Same surface, emitted 56 times.
- Phase 1 of `EmitCxxDefinitions` also walks all header decls per chunk (`announcedDecls` includes
  `headerRoot->decls()`): the "skipped dependent C++ CodeGen declaration" spam repeats ~54x.

**Where the PCH is used and where it is not**
- `EnsureCxxRequestPch` / `-include-pch` are only on the non-incremental fallback in
  `RunCxxTypeRequests` (CFLAT_CPP_INCREMENTAL=0, or a spelling that fails
  `CxxIncrementalSpellingSafe`). The torch sample takes neither branch: 0 PCH builds, 0 PCH loads.
- The incremental path does not need a PCH: the header AST stays resident in the Interpreter.
- clang++ uses no PCH either, so a PCH cannot be part of the cold parity budget.

**Cache written by the cold run:** 439 MB (116 .rq + 117 .json request entries, 61 companion .bc).

---

## 2. Why each type request costs its own chunk + CodeGen today

The call chain (all in cflat/):
- Entry: `ForwardRefScanner.cpp` (:293, :336, :929, :1169, :1199, :1927) and
  `MainListener_Aggregates.cpp:304` call `LLVMBackend::TryRequestCxxType` (CInterop :11427) ->
  `RequestCxxType` (:11668) / `RequestCxxTypeInOwningGroup` (:11773) -> `RequestCxxForeignType`
  (:10414). Also `RequestCxxMemberTypes` (:11403), `RequestCxxSignatureTypes` (:3229) and
  `StdFunctionSpecializationForSpelling` (:2100) from registration.
- `RequestCxxForeignType` is synchronous: its caller (a `ParseDeclarationSpecifiers` copy, or member
  registration) needs the record's layout and members NOW to keep walking. It checks the negative and
  positive request cache (`CxxTypeRequestCacheKey`, :5665), then on a miss runs
  stage 1 then stage 2 via `RunCxxTypeRequests` (:5557).
- `RunCxxTypeRequests`, incremental branch: `GetCxxIncrementalGroup` (:5094, keyed on the import
  line's header set + defines, created once with chunk 0), builds a chunk of text (include prologue
  + unseen prefix + `__cflat_inc_<n>_req_` markers + ODR-use lines), and calls
  `CxxIncrementalGroup::ParseRequest` (CxxIncrementalGroup.cpp :943) -> `Interpreter::Parse(chunk)`
  -> `cflat_cinterop::ExtractCxxIncremental` (CClangExtract.cpp :5329) ->
  `HarvestTranslationUnit` -> `ComputeCxxAbi` (:3924) -> `CreateLLVMCodeGen` (:3937) ->
  `EmitCxxDefinitions` (:4194) -> `WriteBitcodeToFile` (:5009).
- The result is a self-contained `ExtractResult` (records, sigs, bitcode) cached per request and
  later linked by `LinkCxxCompanionModules` (EmitAndLink :2810).

Why per request:
1. **Request = text chunk.** A spelling is answered by parsing marker typedefs / explicit
   instantiations / ODR-use statements through `Interpreter::Parse`. One spelling per chunk keeps
   clang errors attributable (DropBlamedDeclarations, `PrepareRetryChunk`).
2. **Stage 1 / stage 2 split.** Stage 1 needs the member list before stage 2 can write the ODR-use
   lines that force Sema to instantiate member bodies (`BuildCxxRequestOdrUses`).
3. **Companion per request.** Each result must be independently cacheable and replayable warm, so
   each carries its own bitcode - and, because of the shared-root harvest, the whole group surface.
4. **Synchronous callers.** Nothing collects requests before answering them; the ForwardRefScanner
   walk blocks on each.

What the incremental executor already does (and the p3 issues):
- One `clang::Interpreter` per import group per backend, created lazily, kept until
  `EmitExecutable` (`cxxIncrementalGroups_.clear()` in LLVMBackend.cpp :2564). Sema state,
  instantiated specializations and poisoned-function records carry across chunks.
- The Interpreter code-generates each PTU itself (`parsed->TheModule`), which cflat ignores
  (`(void)module` in ExtractCxxIncremental) and then code-generates again with its own CodeGenerator.
  That double work is part of the 7-9 s RunCxxTypeRequests self time and of the 2.3 s Optimizer.
- p3 `cpp-incremental-retry-after-failed-parse-crashes`: an error raised after the
  `hasErrorOccurred()` check (in pending instantiations or HandleTranslationUnit) makes
  `CodeGeneratorImpl` reset its module and `IncrementalAction::GenModule` dereferences null. Known
  triggers are closed; the DropBlamedDeclarations retries remain (17 on a libtorch leg). This is the
  concrete error-recovery hazard the design must not widen.
- p3 `cpp-noinc-mode-std-map-lookup-fails`: the live Sema answers differently from a fresh request TU
  (Sema-side completion across chunks). Order/state dependence is real today.

---

## 3. Design: one Sema, one CodeGen, one companion per import group

Keep the existing `CxxIncrementalGroup` as the one Sema. Change what a request does and when code
is generated.

### 3.1 Requests answer shape only (no CodeGen)
- A type request = the stage-1 chunk only (member list, layout, signatures). Stage 2 is removed as a
  per-request step.
- Request harvest never re-walks the header root: `ExtractCxxIncremental` harvests only the chunk's
  root + announced decls (drop the per-request `HarvestTranslationUnit(headerRoot)` and the shared
  `abiWork`). The group surface is harvested ONCE, in chunk 0.
- Registration becomes lazy for member-signature types: `RegisterCxxClassMembers` records a member
  whose parameter/return is `std::function<...>` (or any not-yet-requested class specialization)
  with its C++ spelling and requests the type at first overload resolution of that member (the same
  "bind on first LOOKUP" rule `TryBindCxxFunction` already applies to free functions).

### 3.2 Batching
- **Pre-collection.** Before ForwardRefScanner runs on a file that imports a C++ group, a cheap
  parse-tree pass collects every type spelling rooted in a C++ namespace of that group (dotted names
  and generic instances: `c10.IntArrayRef`, `torch.nn.Linear`, `std.vector<at.Tensor>`), maps it
  with the existing `TryRequestCxxType` spelling logic, and enqueues it.
- **Round flush.** The queue is answered in ONE chunk: all markers in canonical (sorted) order. Its
  harvest yields member signatures, whose class types become round 2; repeat to a fixpoint (expect
  2-3 rounds on torch). Results land in the existing per-identity memo, so the synchronous
  `RequestCxxForeignType` calls from ForwardRefScanner/MainListener become memo hits.
- **Stragglers.** Spellings only known after type inference (a method's return type) still go
  through the synchronous path, but at stage-1 cost (~70 ms), never with CodeGen.
- **Error isolation in a batch.** If a batch chunk reports errors, split it in halves and retry
  (bisect; the existing `PrepareRetryChunk` / blame machinery works per half). A clean batch - the
  normal case - costs one chunk.

### 3.3 Body instantiation and companion CodeGen: once, at the end, on demand
- Keep ONE `clang::CodeGenerator` per group for the whole compile (either the Interpreter's own
  CodeGen - clang-repl already keeps one CodeGenModule across PTUs and starts a fresh llvm::Module
  per PTU - or one `CreateLLVMCodeGen` created at chunk 0 on cflat's LLVMContext). No per-request
  CodeGenerator, no per-request bitcode.
- During CFlat CodeGeneration, every C++ function/method/ctor/vtable the program module references
  is recorded as a GlobalDecl (the binding already knows the `FunctionDecl` via the harvested
  linkage name). Default-argument wrappers are generated only for callees actually called with
  defaults (train.cb: `backward()`), not for the header's whole surface.
- After CFlat CodeGeneration: one "ODR-use" chunk for the demand set (so Sema instantiates exactly
  those bodies) -> `PerformPendingInstantiations` -> `HandleTopLevelDecl` for each demanded decl ->
  `Release()` one module holding the demanded bodies plus their deferred closure. That is what a
  C++ TU contributes - clang++ -O0 spends 0.04 s in Backend for the same program.
- Link that one module into the program in memory with a single `LinkOnlyNeeded` pass (one module,
  so the non-convergence found with 290 companions does not arise). No serialize/parse round trip
  when the CodeGenerator shares cflat's LLVMContext.
- The error-reach scan (`ErrorReachScan`, body emptying) and `DefineDefaultedSpecialMembers` run on
  the demand set only, before the one CodeGen pass.

### 3.4 Cache keys and warm path on top
- **Group snapshot** (replaces 116 request entries + 61 .bc for this group): key = group key
  (owner headers + header stamps + include dirs + defines + clang args + `kCHeaderCacheVersion` +
  bitfield mode). Content = the bound surface of every request this group has answered (records,
  member sigs, negatives) as one blob, loaded with one read.
- **Companion** keyed on group key + hash of the sorted demand set. Warm: demand set <= cached set ->
  load the one module, link, done (no clang). Demand set grew -> cold path for the delta only
  (create the Interpreter, answer, emit the delta, write a new companion covering the union).
- This is the same "one pre-linked companion per import group + one bound-surface snapshot" shape
  the parallel warm-path run (cflat-fix-perf-warm) is building. The cold path must write exactly
  what that warm path reads; agree the file format with that branch before P3 lands.
- `--check` / LSP: shape-only requests (EDECL); they never create the CodeGenerator.

### 3.5 Header parse overlap
- The `import cpp` line is known at file parse time. Start chunk 0 (`CxxIncrementalGroup::Create`) on
  a worker thread immediately and join at the first request. Runtime import, user-file parse and
  the pre-collection pass then run concurrently with the 4.3 s parse.
- Fold the macro prepass into chunk 0 (a PPCallbacks macro recorder on the Interpreter's
  preprocessor) - saves 0.7 s and a second preprocessing of torch.
- Make the Interpreter's per-PTU CodeGen/backend a no-op (cflat ignores its module): a consumer
  without BackendConsumer's pipeline, or reuse it as the 3.3 generator. Saves the 2.3 s Optimizer
  plus per-chunk CodeGen.

---

## 4. Risks

- **Error recovery in one shared Sema/CodeGen.** A failing request can leave invalid decls that later
  chunks reach. `ModuleBuilder` discards the WHOLE module on any error, so with one companion one bad
  body would drop everything. Mitigations: batch bisect (3.2); the error-reach scan + body emptying
  before the single CodeGen; soft-reset the diagnostic tally before the CodeGen pass (as
  EmitCxxDefinitions already does); the p3 GenModule null-module crash must be closed first (check
  `hasErrorOccurred()` after Parse, before any CodeGen, in cflat's wrapper) because the demand chunk
  runs after many earlier chunks.
- **Request order dependence.** Batching changes the order Sema sees spellings. Known symptoms:
  cpp-noinc-mode-std-map-lookup-fails, cpp-candidate-tier-differs-cold-vs-warm. Mitigate with
  canonical (sorted) batch order, and a cold-vs-warm equality check on the fixtures (the bound
  surface from the snapshot must match a fresh cold run byte for byte).
- **Lazy member typing changes answers.** A member bound later may resolve overloads differently
  than one bound eagerly. Guard with the existing interop fixtures + test_libs tier 1-2.
- **LSP pool concurrency.** Groups live per backend; 4 pool slots x a torch Interpreter = ~8 GB.
  Sema is not thread-safe, so a shared Interpreter needs a per-group mutex (requests serialize) or
  the LSP stays shape-only and reads the group snapshot. Plan: LSP never creates the CodeGenerator
  and prefers the snapshot; decide sharing vs per-slot after measuring LSP memory.
- **Windows.** MSVC ABI: vtables are not key-function anchored (DefineMicrosoftVTableMembers),
  dllimport/COFF comdats in one merged companion, MS mangling of chunk-local helpers, the Interpreter
  must never build an executor (ParseOnlyExecutor stays). Header-group coverage differs (windows.h
  groups). Concrete risk: the single LinkOnlyNeeded pass over a COFF companion with comdat-any
  selection; test.bat Release is the Windows gate for P2-P3.
- **Worker-thread parse.** clang is fine on a non-main thread, but `llvm::TimeTraceScope` and
  CompilerManager's crash handlers are per-thread; the crash dump must still name the header.
- **Memory.** One AST + one CodeGenModule for the compile: roughly what exists now minus 60 dead
  LLVMContexts. Released before EmitExecutable as today.

---

## 5. Staged phases (torch sample, cold, idle machine)

Baseline 94-98 s. Numbers are estimates from the measured breakdown; each phase's acceptance is
parity.sh cold median of 3 on train.cb, plus test.sh green and test_libs tiers 1-2 green (never
tier 3 in the gate), plus the listed counters from `-v` / `-ftime-trace`.

| Phase | Change | Saves (torch) | Expected cold | Acceptance |
|---|---|---|---|---|
| P0 | parity.sh into repo as gate; `-v` summary: chunks, CodeGen passes, companion bytes | 0 | 94 s | counters print |
| P1 | Lazy member-signature types at registration (std::function et al.) (3.1) | ~51 s (B) | ~43 s | 0 std::function requests on train.cb; RegisterCRecords < 2 s; <= 14 requests total |
| P2 | Request stage 2 stops re-harvesting/re-emitting the group surface (3.1); header walk only in chunk 0 | ~12 s of C + ~7 s link | ~24 s | per-request companion < 300 KB; CxxDefinitionEmit total < 3 s outside chunk 0; LinkCxxCompanion < 2 s |
| P3 | One persistent CodeGenerator per group, demand-driven end-of-compile emission, one in-memory companion; default wrappers on demand; request cache writes group snapshot + one companion (3.3, 3.4) | header companion 4.1 s + wrapper chunk 3.7 s + stage-2 chunks ~3 s + store/link ~2 s | ~11 s | exactly 1 companion module; companion defs < 5,000; 0 per-request bitcode; warm branch reads it (joint check) |
| P4 | Batched rounds from pre-collection (3.2); stage 2 gone | ~24 chunks -> ~3 chunks, ~1.5 s; registration replay ~1.5 s | ~8 s | <= 4 incremental chunks after chunk 0 (counter); cold == warm bound surface on interop fixtures |
| P5 | Header floor: prepass folded into chunk 0, Interpreter per-PTU backend off, chunk 0 on a worker thread overlapped with runtime import + user parse + pre-collection (3.5) | prepass 0.7 + optimizer 2.3 + ~0.8 overlap + ~2-3 s CHeaderExtract self (lazy JSON/record mapping of the unused surface) | **<= 4.6 s** | parity.sh cold <= 1.1x clang++ median of 3 |
| P6 | Next sample: simdjson, then fmt, json, eigen - same gate, one sample at a time | - | - | each <= 1.1x cold |

Floor check: chunk 0 (4.3 s parse + 1.0 s instantiation) equals clang++'s whole compile, so P5 only
reaches parity if everything else is under ~0.3 s serial or overlapped with the parse. The main
residual risks are the ~3 s of CHeaderExtract self time (mapping the 88k-definition surface to CFlat
records/sigs), which must also become lazy in P5, and the demand chunk's instantiation cost (bounded
by clang++'s own 0.86 s PerformPendingInstantiations for the same program).

### Status: Windows baseline, all C++-interop samples (2026-09-29, master c4fda05a)

Harness: `test_libs_parity.ps1 -N 3` (repo root; Windows counterpart of parity.sh). It covers every test_libs
C++-interop case with a `<case>.cpp` twin: fmt (4), json (3), simdjson (2) and torch (11). Both baselines:
clang++ -O0 compile+link vs cflat `-o` cold/warm, and clang++ -fsyntax-only vs `cflat --check`. Idle machine,
pinned 0x55. Full per-case table + method: internal/issue/p1/cpp-import-compile-time-parity-with-clang.md
("Windows full baseline incl. libtorch").

| sample | clang++ link | cold (x clang++) | warm (x clang++) | check cold vs -fsyntax-only |
|---|---|---|---|---|
| simdjson | 1.33-1.40 s | 1.05-1.19x | 0.16-0.21x | 1.12-1.26x |
| fmt | 0.67-0.68 s | 1.22-1.38x | 0.24-0.25x | 1.32-1.41x |
| torch (11 cases) | 9.6-10.7 s | 1.64-1.82x | 0.19-0.25x | 1.63-1.76x |
| json | 1.98-2.11 s | 1.66-2.00x | 0.15-0.26x | 1.76-1.92x |

- **Warm target met on every sample.**
- **Cold is <= 1.1x only for simdjson_01.**
- **Front-end/interop gap.** The syntax-only ratio tracks the link ratio, so the remaining cold gap is
  front-end/interop work, not codegen/link.
- **Windows torch vs macOS.** Windows torch cold is 1.74x vs 1.6x on macOS train.cb (plain LLVM).
Perf changes landed on master (newest first; torch train.cb = macOS sample, others = Windows test_libs):

| commit | change | effect |
|---|---|---|
| c4fda05a | incremental group: drop already-listed ParmVarDecls before late body parse; `__cflat_std_function_ctor_` rename-on-redefine | fixes a Windows hang (decl-chain cycle) and a cache-replay redefinition; torch tier 3 enabled |
| 79804a83 | per-call memo in the mangled-type parser (was exponential on unknown template arity); IsCxxSharedPtrUpcast shape reject before demangling; CxxGroupHeaderHash memo on (path, mtime, size) | json_01 warm 1.31 -> 0.56 s; simdjson_02 cold 1.72x -> 1.15x |
| 1898482a | round 3: by-value gate projects only dtor + copy/move ctors; macro prepass folded into chunk 0; error-body sweep only when an error can exist; callback ABI by-value slice | train.cb cold 6.54 -> 5.26 s (1.6x; 1.2x with PGO+ThinLTO LLVM), warm 0.47 s |
| 3e0d5d8e | rounds 1-2 (P1-P3): one demand-driven companion per group; lazy std::function binds; parse-only requests; lazy record projection; skipped/late-parsed inline bodies; warm-edit demand replay; cold glue | train.cb cold 98 -> 6.5 s, warm 17.6 -> 0.55 s |
| 1405f9f7 | skip rebound signatures, replay seeded namespaces, LSP reads the compile cache | - |
| 1f2f3f7e | lazy companion promotion, merge-then-LinkOnlyNeeded companion link | - |
| cfdbeca4 | request cache entry holds only what its key's headers include | - |
| 62c21233 | unified C++ reparse switch, versioned header cache | test suite 413 -> ~125 s |
| 9b825a07 | C++ incremental requests on by default (one Interpreter TU per import line) | cold budget 1 / warm 0 enforced |
| 75617db5 | header extraction is chunk 0 of the group Interpreter | simdjson cold 4.7 -> 2.8 s |

- **Next lever to measure:** gate the `__cflat_use*` ODR-use helpers to used members.
- **Last step:** a PGO-built LLVM.

---

## 6. Prior art: keeping one Sema alive

- **clang-repl / clang::Interpreter** (what cflat already uses): `IncrementalParser` feeds each input
  as a PartialTranslationUnit into one Sema; `IncrementalAction` keeps ONE CodeGenerator/
  CodeGenModule and calls `StartModule` per PTU, moving deferred decls forward. `Undo(n)` rolls back
  whole successful PTUs only; a failed parse leaves no PTU to undo (the p3 crash). Lesson: reuse its
  one CodeGenModule instead of a second generator; guard the post-parse error window.
- **cling** (ROOT): same shape, older. Each input is a `Transaction`; `DeclCollector` records
  decls; `DeclUnloader` removes a failed transaction's decls (the recovery clang-repl lacks).
  `LookupHelper` answers "does type X exist / what is its layout" through Sema lookup and template
  instantiation APIs without parsing text (findType/findScope/findFunctionProto). Lesson: answer
  shape requests via Sema APIs where the spelling allows; keep text chunks for the rest.
- **Swift ClangImporter**: one clang CompilerInstance per Swift module compile, imported lazily -
  a clang decl is imported only when Swift name lookup reaches it (the "bind on first lookup" of
  3.1). IRGen owns one `clang::CodeGenerator` (`CreateLLVMCodeGen`) for the whole compile and hands it
  only the clang decls Swift code references; its module is finalized once and merged into the
  Swift module. C++ interop instantiates templates on demand in that same Sema. Lesson: this is 3.3
  almost exactly - lazy import, demand-driven emission through one generator, one merge.
- **Zig translate-c / @cImport**: one clang parse (ASTUnit) per @cImport block, whole-TU translation
  to Zig source, cached by content hash of the generated C source + flags. C only (no templates, no
  bodies to instantiate), so no persistent Sema is needed. Lesson: the cache granularity is the whole
  import block, not per-symbol - matching the group snapshot in 3.4.
