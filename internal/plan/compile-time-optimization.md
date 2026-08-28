# Compile-time optimization

Opened 2026-08-28. Measured, multi-change, iterative. Landing in risk order: the mechanical,
provably-behaviour-preserving items first (tier 1), the front-end walker rewrites second (tier 2).

## Baseline (macOS arm64, Release, warm `--init-local` cache)

| Measurement | Value |
|---|---|
| `./test.sh Release -j 1` | **140s** |
| `./test.sh Release` (`-j 18`) | 61s (only 2.3x - the serial discovery pass is an Amdahl floor) |
| `cflat --version` (process startup floor) | 10ms |
| hello-world `--check` | **40ms** |
| hello-world `-o out.bin` | 284ms |

Where the 140s goes:

| Bucket | Time |
|---|---|
| 326 `err_*.cb` cold `--check` | 33s (p50 61ms/file) |
| 326 `err_*.cb` warm `--check` | 34s |
| 41 `test_*.cb` compile+link+run | 37s (p50 0.6s, max 2.8s `test_move`) |
| serial pseudo-locale discovery (326 files, one process) | 12s (37ms/file) |
| policy discovery loop + `--init-local` + asan regression | ~24s |

So ~80% of the suite is small-file front-end work, where FIXED per-compile overhead dominates.

## Profiles - two different shapes

Method: `-ftime-trace` for phase attribution, `sample <pid> N 1` for self-time attribution.

**Small file (`--check`, sampled over the 326-file discovery batch, 8361 samples):**

| Site | Share |
|---|---|
| `LoadCoreBitcodeIfFresh` | **45%** - `rejoinRuntime`->`weakly_canonical`->`realpath` **21%**, meta.json parse 9%, bitcode parse 7% |
| `RunBaselinePasses` over the core module | **16%** |
| codegen walk | 13% |
| `RecordDependency`->`weakly_canonical` | **11%** |
| ANTLR parse | 4% |

**Large file (`Test/test_move.cb`, 11.7k lines, 2.66s):** CodeGeneration 48%, ForwardRefScan 17%,
Parse 16%, EmitExecutable 12%. Flat self-time says **53% of the whole compile sits inside
libc++abi `__dynamic_cast`** (`type_info::operator==` alone is 16%).

Takeaway: small compiles are bound by core-cache load + filesystem canonicalization; large compiles
are bound by `dynamic_cast` in the two parse-tree walkers.

---

## Tier 1 - mechanical, behaviour-preserving (do first, commit, then reassess)

### 1. Memoize `rejoinRuntime` in `LoadCoreBitcodeIfFresh`

`cflat/LLVMBackend.cpp:7184`. The lambda calls `std::filesystem::weakly_canonical` once per cached
core symbol while restoring `coreSymbolIndex_`. The cache holds **1038 `core_symbols` + 324
`core_variables` = 1362 entries resolving to 25 DISTINCT relative paths** - i.e. 1362 `realpath()`
syscall chains per compile, 98% of them redundant. Measured at **21% of every small compile**.

Fix: an `unordered_map<std::string, std::string>` memo local to the function, keyed on the raw
relative path. Pure caching - the output is identical.

Follow-on (same site, larger win, slightly more thought): the restored entries only ever feed
`coreSymbolIndex_`, which is consumed by `symbolSink_->MergeFrom(coreSymbolIndex_)`
(`LLVMBackend.cpp:3953`, LSP only) and the `--symbol` lookup. Guarding the whole
`CoreDes:Symbols` / `CoreDes:Variables` restore on "a consumer exists" would remove the work
entirely AND most of the 440KB meta.json parse (another 9%). Deferred to a second pass - it needs
the `--symbol` path audited, whereas the memo is unconditionally safe.

### 2. Skip `RunBaselinePasses` under `--check`

`cflat/LLVMBackend.cpp:2329`. `--check` emits no object, no IR and no executable, but still runs
SROA / mem2reg / instcombine / simplifycfg over the entire core module. The gate today is only
`if (!args.hasFlag("no-opt"))`.

Fix: also skip when `checkOnly` is set, together with the `OptimizeModule` / `RunGlobalDCE` arms
that follow it. NOTE: `--isolated` IS a downstream consumer - see "Correction to item 2's premise"
below for the gate that actually landed.

Measured: the 326-file batch goes **12.1s -> 10.1s (-17%)** with `--no-opt`, and exit codes are
byte-identical across all 326 error tests. ~11s off the suite on its own.

### 3. Memoize + de-quadratic `RecordDependency`

`cflat/LLVMBackend.cpp:173`. Per call it does `weakly_canonical` AND `is_regular_file` (two syscall
chains), then dedups with a linear `std::find` over `dependencyFiles_` (O(n^2)). Measured at **11%
of every small compile**.

Fix: memoize raw-path -> canonical-text; add an `unordered_set<std::string>` beside the vector for
the membership test. **`dependencyFiles_` order must be preserved** - it is written to the
`.cflat-dep.json` manifest - so keep the vector as the ordered store and use the set only for the
lookup. Both must be cleared wherever `dependencyFiles_.clear()` happens
(`LLVMBackend.cpp:1592`) and in `ResetForReanalysis`.

### Tier 1 acceptance

- `./test.sh Release` green (720 passed, 0 failed, 8 skipped).
- The 326-file `--check` batch drops from 12.1s; record the new number here.
- `-j 1` suite wall time drops from 140s; record the new number here.
- No diff under `cflat/locales/` other than what the generator produces.

### Tier 1 results (measured 2026-08-28, warm cache, Release)

| Measurement | Before | After |
|---|---|---|
| hello-world `--check` | 40ms | **20ms** (-50%) |
| 326-file `--check` batch | 12.1s | **7.7s** (-36%) |
| `./test.sh Release` (`-j 18`) | 61s | **37s** (-39%) |
| `./test.sh Release -j 1` | 140s | **117s** (-16%) |

Suite green: 720 passed, 0 failed, 8 skipped.

Per-item attribution on the batch: item 2 (`--check` skips the optimize block) 12.1 -> 10.1s,
items 1+3 (the two canonicalization memos) 10.1 -> 7.7s.

The parallel run gains more than the serial one (-39% vs -16%) because the removed work is
dominated by `realpath()` syscalls, which contend when 18 compilers run at once.

### Correction to item 2's premise

The plan claimed nothing downstream of `--check` consumes the module. That is FALSE for
`--isolated`: `AuditIsolatedModule()` audits the OPTIMIZED module, and gating the block on
`checkOnly` alone deterministically fails six policy tests. The landed gate is
`if (!checkOnly || isolatedPolicy_)`. Do not "simplify" it back.

---

## Tier 2 - front-end walkers (riskier, separate change)

### 4. Replace `dynamic_cast` with rule-index dispatch in the hot walkers - LANDED 2026-08-28

`dynamic_cast` on ARM64 libc++abi walks the RTTI hierarchy. After item 5 landed, a re-profile of
`Test/test_move.cb` still put ~21% of the compile in `__dynamic_cast` machinery, CONCENTRATED in
four free functions in `MainListener.h` - 124 of ~135 attributed samples:
`CollectUnconditionalMovedNames` (57), `CollectConsumedStoreNames` (44),
`SubtreeContainsFunctionReturn` (18), `CollectOwnReturnExpressions` (5).

Landed fix: a `RuleIndexOf<Ctx>` trait plus `AsRuleCtx<Ctx>(node)`, which tests
`getTreeType() == ParseTreeType::RULE` (a stored field, free) then compares `getRuleIndex()` to a
compile-time constant. Binding the index to the context TYPE via the trait makes a mismatched
pair a compile error rather than a bad `static_cast`. Converted ONLY those four functions - the
other ~40 `dynamic_cast`s in `MainListener.h` never showed in a profile, and each one converted is
risk without measured benefit.

LOAD-BEARING INVARIANT, also recorded on the helper: this is exact only because `CFlat.g4` has NO
labeled alternatives (`# Label`). ANTLR generates a context SUBCLASS per labeled alternative, and
that subclass shares its parent's rule index - `dynamic_cast` distinguishes the two, `AsRuleCtx`
cannot. Adding a labeled alternative to the grammar silently breaks every `AsRuleCtx` call site.

Results (this change alone, controlled warm-cache A/B - see the methodology note below):

| Measurement | Before | After |
|---|---|---|
| `Test/test_move.cb` compile | 1.64s | **1.29s** (-21%) |
| `./test.sh Release` (`-j 18`) | 33s | 36s (noise; small files dominate) |

Suite green: 720 passed, 0 failed, 8 skipped. IR identical across all 39 files.

Remaining after this: `dynamic_cast` is no longer concentrated anywhere - the ~40 unconverted
sites are spread thin. The next lever is the `LoadCoreBitcodeIfFresh` follow-on in item 1
(skip the `CoreDes:Symbols` restore and most of the 440KB meta.json parse when no symbol sink
exists), not more walker work.

---

## MEASUREMENT METHODOLOGY - read before benchmarking this compiler

Both the timing AND the emitted IR depend on whether the **core bitcode cache hits**, and
`cmake_build.sh` redeploys `core/`, which rotates the cache-directory hash. A build can therefore
land on a populated cache dir (hit) or a fresh one (miss), and the two give:

- **Timing**: `test_move` measured 1.74s on a miss vs 1.29s on a hit - a 35% swing with NO source
  change. A miss re-parses the whole core library.
- **IR**: a hit reconstructs the module from bitcode, a miss builds it by parsing core. The two are
  semantically equivalent but differ in global emission order (`@__FILE__` position) and SSA value
  numbering (`%.unpack4` vs `%.unpack7`). This shows up as ~150 diff lines per file.

Verified directly: two consecutive builds of IDENTICAL source produced IR differing on all 39/39
files and timings of 1.74s vs 1.30s.

So ALWAYS run `cflat --init-local` after a build and before measuring or capturing IR, and confirm
`-v` prints `core bitcode cache: hit` on both sides. `scratch/ab_measure.sh` in the tier-2 work did
this; with it, the baseline-vs-changed IR diff was 0/39 and the timings were stable to +/-0.02s.

A raw before/after IR diff WITHOUT this warm-up step is not evidence of anything.

### 5. Fix the O(n^2) in `CollectUnconditionalMovedNames` - LANDED 2026-08-28

It called the full-subtree `SubtreeContainsFunctionReturn(child)` for EVERY child at EVERY
recursion level, so a node was re-scanned once per ancestor on the descend path; the sample showed
a ~30-deep self-recursive chain each re-scanning the tail.

Landed fix: memoize `SubtreeContainsFunctionReturn` on a `ReturnScanMemo`
(`unordered_map<const ParseTree*, bool>`) threaded through both walkers, with ONE memo created per
`ApplyOwningSinkInferenceToBody` call. The function is a pure function of the subtree - it does not
read `evalIfConst` or any compiler state - so memoization is behaviour-identical by construction,
and scoping the memo to a single call means no stale pointer can survive a reparse. O(n * depth)
becomes O(n).

The "skip expression-level children" half was deliberately NOT done: skipping a scan that would
have returned true silently changes move-sink inference, and memoization already collects the win.

Results (this change alone, on top of tier 1):

| Measurement | Before | After |
|---|---|---|
| `Test/test_move.cb` compile (11.7k lines) | 2.63s | **1.64s** (-38%) |
| `Test/test_basic.cb` compile | 1.42s | **0.75s** |
| `Test/test_collection_leaks.cb` compile | 2.65s | **1.76s** |
| `./test.sh Release` (`-j 18`) | 37s | **33s** |
| `./test.sh Release -j 1` | 117s | **110s** |

Suite green: 720 passed, 0 failed, 8 skipped.

`test_move` sample count fell 2342 -> 1422 and `SubtreeContainsFunctionReturn` left the profile
entirely. The suite moves little because it is dominated by small files, where fixed per-compile
cost - not the walkers - is the bound.

VERIFICATION NOTE for future changes in this area: a green suite is NOT sufficient evidence for a
change to ownership / move-sink inference. Capture the emitted IR for every compilable
`Test/test_*.cb` with `-o ... --out-lli ...` before the change and `diff` after; all 39 files must
be identical apart from the leading `; ModuleID` line. CAVEAT: this item's IR check was run before
the cache-state effect below was understood, so it happened to compare two cache-HIT runs by luck
rather than by design. See "MEASUREMENT METHODOLOGY" - warm the cache with `--init-local` on both
sides or the diff is meaningless.

---

## Tier 2b - fixed-cost work per compile - LANDED 2026-08-28

### A. `RecordDependency` was quadratic in batch mode

`Compile()` recorded EVERY positional on EVERY call. Batch `--check` calls `Compile(args, file)`
once per file with all 326 positionals still on the command line, so the error suite did ~106k
`realpath`+`stat` pairs. The tier-1 memo could not help: within one `Compile()` the 326 paths are
all distinct, and `ResetForReanalysis` clears the memo between files.

Fix: when `inputOverride` is non-empty, record only that file.

The changed branch is unobservable by design - `inputOverride` is set only by the batch `--check`
loop in `main.cpp`, which emits no output, and the single consumer of `dependencyFiles_`
(`GetDependencyFiles`, `main.cpp:642`) sits on the single-compile path outside that loop. The
manifest check below therefore guards the branch that was NOT changed, which is exactly the point:
it proves ordinary `-o` compiles still record every positional.

### B. Split the LSP symbol index out of the core cache metadata

`core_symbols` + `core_variables` were 291 KB of a 430 KB `core_<platform>.meta.json`, parsed
eagerly by `llvm::json::parse` on every cache load including plain CLI compiles that never look at
a symbol index. Guarding only the restore *loop* would not have helped - the DOM is built before
any loop runs, so the data had to physically leave the file.

They now live in `core_<platform>.symbols.json`, read only when `symbolSink_ != nullptr`. Cache
format version 8 -> 9 so existing caches rebuild rather than silently losing their symbol index.

Measured: `core_macos.meta.json` 430,347 -> 164,102 bytes (-62%); sidecar 276,245 bytes.

**The `symbolSink_ != nullptr` gate is safe because** all three consumer paths install the sink
immediately before `Analyze()` (`LspServer.cpp:1655`, `SymbolQuery.cpp:242`, `SymbolQuery.cpp:1017`),
and `Analyze()` already gates the matching `symbolSink_->MergeFrom(coreSymbolIndex_)` on the same
condition. `LoadCoreBitcodeIfFresh` re-runs per `Analyze()` and clears `coreSymbolIndex_` each time,
so read and write are always gated together.

**LOAD-BEARING: every failure exit in `LoadCoreBitcodeIfFresh` must precede the context/module swap.**
The first draft read and parsed the sidecar at the restore site and returned false on failure - i.e.
reported a cache MISS on a backend whose module and tables were already fully deserialized, after
which the caller re-imports core on top of it. The sidecar read was hoisted above the swap for this
reason. Do not move it back down for tidiness.

### Tier 2b results (warm cache, Release)

| Metric | Before | After |
|---|---|---|
| 326-file `--check` batch | 5.72s | 4.09-4.18s (-28%) |
| hello-world `--check` floor | 0.02s | 0.02s (unchanged) |
| `test.sh -j 18` | 33s | 28-30s |
| IR identity | - | 0 / 39 files differ |
| dependency manifest | - | 34 / 34 inputs identical |
| `test.sh Release` | - | 720 passed, 0 failed, 8 skipped (x2) |
| `test_lsp.sh Release` | - | All LSP tests passed |

`test_lsp.sh` is mandatory for any change in this area - it is the only suite that exercises the
symbol-index path that item B gates.

---

## Large-object copy elimination - MEASURED NULL RESULT 2026-08-28

Investigated because the profile attributes ~25% of small-file self time to the allocator, and
the hot types are big:

| Type | sizeof |
|---|---|
| `NamedVariable` | 1216 B |
| `DeclTypeAndValue` | 432 B |
| `TypeAndValue` | 320 B |

`NamedVariable` embeds `TypeAndValue` plus several `std::string`s and a vector, so each copy is
several mallocs.

Found and fixed real deep copies: `ComputeOverloadFunction` took
`vector<pair<vector<NamedVariable>, FunctionSymbol>>` BY VALUE (both call sites pass lvalues, so
the whole candidate set was deep-copied per overloaded call); `CreateFunctionDeclaration`,
`CreateFunctionDefinition` and `CreateLocalVariable` took `TypeAndValue` / `vector<TypeAndValue>`
by value; and two `resolvedCandidate.emplace_back(matched, ...)` copied a dead vector.
All converted to `const&` / `std::move`.

**Result: no measurable improvement.** 326-file batch 4.17s vs 4.09-4.18s baseline; `test_move`
1.26-1.28s vs 1.29s; hello-world floor unchanged at 0.02s. IR 0/39 differences, both suites green.
The copies were real but not hot enough to show. Do not re-investigate this angle expecting a win.

### What NOT to retry

- **`CreateGlobalVariable` must stay by value** - its body does
  `typeValue.GuardedBy = pendingGlobalGuardedBy;`.
- **`SetStackVariable` / `RegisterFunctionArgument` are not bugs.** They take `NamedVariable` by
  value and `std::move` internally - the correct sink idiom.
- **`-Wlarge-by-value-copy` cannot automate this sweep.** It fires only for trivially-copyable
  types, so it silently skips every type in the table above except the POD-ish ones; a clean build
  under it is a false negative, verified against a synthetic test. `-Wrange-loop-construct` did not
  fire on an obvious copy either.

### The aliasing rule these conversions had to clear

A by-value parameter also protects against aliasing. `CreateFunctionDeclaration`'s body does
`functionTable[functionName].push_back(funcSym)` and then keeps reading `arguments` afterwards -
so if any call site ever passes a `FunctionSymbol::Parameters` owned by `functionTable`, the
`const&` dangles on reallocation. Verified safe by extracting the argument expression at all 66
`CreateFunctionDeclaration` / `CreateFunctionDefinition` call sites: every one passes a braced
temporary or a plain local (`allParams`, `wrapperParams`, `params`, `e.params` from a local
`CSigEntry` list). **Re-run that check before adding a call site that passes a registry-owned
vector.**

---

## Tier 3 - suite-level, not the compiler

The pseudo-locale discovery pass is 12s serial and the policy discovery loop is ~13 serial 0.7s
invocations. Sharding discovery across jobs would cut the `-j 18` wall further without touching the
compiler. Independent of tiers 1 and 2.

## ANTLR prediction - SLL null result + conditionalExpression left-factor - 2026-08-28

Measured with a temp env-gated ProfilingATNSimulator hook (recipe: scratch/parse_profiler.patch,
apply to LLVMBackend.cpp, set CFLAT_PARSE_PROFILE=1, aggregate PARSEPROF stderr lines).
test_move parse prediction was 380ms of a 1.28s compile (~30%).

Landed: left-factored conditionalExpression ('?' / '??' merged into one optional tail after
logicalOrExpression). Accessors unchanged, zero listener edits. Prediction 380 -> 295ms;
test_move 1.27-1.28 -> 1.16s (-9%); err batch 4.14-4.16 -> 3.90s (-6%). IR 0/40 diff,
test.sh 720/0/8, LSP green.

Do NOT retry:
- PredictionMode::SLL two-stage (SLL+bail, LL fallback): measured null. The cost is DFA
  construction (closure_), which SLL pays identically; full-context escalation was not the
  bottleneck. Implemented, A/B'd 0ms delta, reverted.
- Left-factoring parameterDeclaration (declarator vs abstractDeclarator alts): behavior-CHANGING.
  The two-alt form is load-bearing: full-context prediction is what stops the greedy
  declarationSpecifiers loop before the parameter name ('i32 fd' otherwise swallows 'fd' as a
  specifier -> "Function parameter name is missing"). Its 26ms is the price of that resolution.

Remaining prediction profile (post-factor): assignmentExpression decision 116ms (39%),
postfixExpression 35ms (7k LL fallbacks), parameterDeclaration 26ms (see above).
Next candidate: restructure assignmentExpression from
  unaryExpression assignmentOperator assignmentExpression | conditionalExpression
to the standard
  conditionalExpression (assignmentOperator assignmentExpression)?
Tree-shape CHANGE: assignment LHS becomes a conditionalExpression node; ~22 AssignmentExpression
+ ~15 assignmentOperator listener sites must adapt, and invalid-LHS (a+b = c) moves from syntax
error to a listener LogError. Est. up to ~100ms (~8%) on parse-heavy compiles. Not started.

## ANTLR experiment timebox - RESULTS 2026-08-28 (2h, concluded)

Landed (both in CFlat.g4, zero listener edits, accessors preserved):
- conditionalExpression left-factor (earlier same day; see section above).
- selectionStatement left-factor: 'if' 'const'? '(' ... merged from two alts. Sole consumer
  sel->If() && sel->Const() (MainListener.h:1952) unchanged. Selection prediction 50.3ms ->
  1.2ms in the worktree A/B; 6-file IR diff identical; suite green.
Final serial numbers (both factors): test_move 1.15-1.16s (from 1.27-1.28 baseline, -9%),
err batch 3.81-3.82s (from 4.14-4.16, -8%). IR 0/40 vs pre-change capture, test.sh 720/0/8,
LSP green.

Do NOT retry (adds to the list in the section above):
- assignmentExpression restructure to 'conditionalExpression (assignmentOperator
  assignmentExpression)?' (the standard ANTLR-Java shape): NET LOSS, fully implemented,
  measured, reverted. assignmentExpression prediction fell 116 -> 9.3ms, but the C
  declaration-vs-expression ambiguity it was absorbing moved into blockItem (9.6 -> 149ms;
  'T* p = x;' becomes a valid expression, so blockItem needed assignment-free expression
  alternatives before declaration to keep tie-breaks). Totals: 295ms before, 325ms after
  (316ms best variant); test_move 1.16 -> 1.24s. The prediction cost is the PRICE of the
  ambiguity, conserved under grammar restructuring - same lesson as parameterDeclaration.
  Also required narrowing simdTypeSpecifier's lane count to shiftExpression because
  '(4>mask)=a' became grammatical. Full working diff preserved at scratch/assign_full.diff
  (hardened variant: assign_verify.sh-era tree) if the shape is ever wanted for language
  reasons; do not re-do it for performance.
- postfixExpression loop (35ms, ~7k SLL->LL fallbacks): conflicts are loop-enter vs loop-exit
  on '(' 7016 / '.' 4029 / '[' 1121 / '->' 887 across 4 test files (ContextSensitivityInfo
  dump). Only fix is a recursive postfixSuffix restructure that changes every accessor -
  investigated, not attempted; revisit only with a language-level reason.

Remaining prediction profile after both factors (test_move): postfix 35ms, parameterDecl 26ms
(load-bearing), declarationSpecifiers 17ms (load-bearing), genericIdentifier 13ms (volume),
initializer/fieldInit ~17ms (deliberate order-resolved ties). ANTLR prediction is now ~245ms
of a 1.16s compile; the cheap grammar wins are exhausted.

## LLVM-side fixed-cost audit (opus subagent, read-only) - 2026-08-28

Full ranked list from a call-graph decomposition of scratch/p_small.txt / p_large.txt.
Batch (--check x326, 3.9s) opportunities, deferred with the batch bucket by user ruling:
 1. Lazy core bitcode materialization (getLazyBitcodeModule, --check/LSP only) ~16.8%
 2. Memoize core meta.json DOM across batch files (parse 12.4% + destroy 1.9%) ~14.2%
 3. Discard local value names on cache load (context->setDiscardValueNames one-liner) ~4.5%
 4. Scope VerifyModule to functions defined by the TU instead of whole core per file ~4.6%
    (LLVMBackend.cpp:2344 unconditional-in-Release; blanket #ifndef NDEBUG rejected as it
    moves malformed-IR detection out of the gating suite; scoped variant recommended)
 5. Memoize ComputeCoreHash + weakly_canonical(runtimeDir) per process ~2.8%
 6. Write "Debug Info Version" module flag in SaveCoreBitcode to skip StripDebugInfo walk ~0.6%
 7. Mid-batch Module teardown 6.8% - do NOT attack directly; falls out of item 1.
Combined low-risk (2,3,5,6) ~22% of batch; with 1 + scoped 4: ~40%.

Single-compile findings:
 8. ~20% of a 1.16s test_move -o compile is MachineFunctionPass/SelectionDAG over the WHOLE
    core library linked into every exe (EmitExecutableMachO 24.8%). Next step is measurement
    only: count isel'd functions that are core-origin post-GlobalDCE; if core dominates,
    cache a precompiled core OBJECT next to core_macos.bc and link it. High risk, structural.
 9. Free deletion, no ruling needed: LLVMBackend_OwnershipTemps.cpp:963 calls
    llvm::verifyFunction on every fresh body-less prototype and DISCARDS the result.
 10. Profile caveat: the p_large test_move -o run was cache-COLD (LoadCoreBitcodeIfFresh 0
    samples; core arrived via CompileImportedFile). Why -o missed the warm cache there needs
    a follow-up check before trusting single-compile cache assumptions.

## Core sharing / sharding design notes - 2026-08-28 (investigation, no code)

Question: can the core library stop being re-loaded (batch) and re-codegen'd (per exe), given
each compile mutates the module it lives in?

Measurements (Release, warm cache, LLVM 23 llvm-nm on core_macos.bc = 580 defined symbols):
- hello-world -o: 0.06s total; final IR has 38 defines / 1157 lines. GlobalDCE already strips
  core hard for small programs.
- test_move -o (1.16s): 911 defines, of which only 69 are pristine-core (1.7k of 39.7k IR
  lines, ~4%). The isel weight is user functions + core-template MONOMORPHIZATIONS
  (list<UserT> etc.), which are program-specific by construction.
- REVISES the LLVM audit item 8: a precompiled core OBJECT would save ~4%, not ~20% - the
  audit could not separate core from monomorphs in sample data; the symbol intersection can.
  Do not build core-object caching for performance. Pre-instantiating common builtin-typed
  generics (list<i32>, list<string>...) in the cache is the surviving variant of that idea,
  but test_move showed ~1 builtin-typed instantiation vs ~20 user-typed - needs measurement
  over real programs (example/ tree) before any design work.

Why the module cannot simply be shared across batch files: user codegen appends functions,
globals, and string-pool entries into the SAME llvm::Module the core was loaded into, and all
symbol tables (functionTable etc.) hold llvm::Value* into it; ResetForReanalysis swaps
context+module wholesale. A two-module design (immutable core + per-file user module with
external declarations) founders on cross-module Value*: every table entry pointing into the
core module becomes invalid for CreateCall in the user module. Large refactor, rejected while
the reload can instead be made nearly free.

What --check actually needs from core (verified in code):
- returnBlockTable stores CompoundStatementContext* (parse contexts) - return-block inlining
  regenerates from source, never reads core IR bodies.
- Baseline passes / DCE / codegen are gated off under checkOnly (LLVMBackend.cpp:2352).
- The ONLY body consumer left is the unconditional VerifyModule (2344) - already flagged as
  audit item 4 (scope it to TU-defined functions).
So for --check/LSP, core function bodies are dead weight end to end, and "sharding" reduces to
the audit's low-risk batch items: memoized ComputeCoreHash (5) + cached meta.json DOM (2) +
lazy bitcode module (1) + scoped verify (4) = the reload becomes metadata-deserialize only
(~1ms-shaped), no architectural change, est ~35-40% of the 3.8s batch. That combination IS the
recommended "avoid contention" design; implement in that order (5, 2 first: trivial and
independent; then 4; then 1 with the materialize-on-emit guard for -o/-l paths).

## Batch items 5+2+4+9 - LANDED 2026-08-28 (timebox 2)

Implemented (audit items from the LLVM-side section above): ComputeCoreHash + canonical-path
memo (member cache; cleared by ResetForReanalysis unless the pre-existing batchMode_ flag is
set - SetBatchMode came with --check batch, reused); meta.json + symbols.json DOM caches
(CoreJsonCacheEntry: buffer declared before Value - the DOM holds StringRefs into it; keyed
path+core_hash+mtime+size; survives ResetForReanalysis like parseTreeCache_, comment says so);
scoped VerifyModule (cachedFunctionNames_ snapshot after cache load, verifyFunction on
non-snapshot definitions only, full verifyModule on any cold/miss path - snapshot reset at
Compile/Analyze entry so a miss can never inherit one); deleted the discarded verifyFunction
on body-less prototypes (LLVMBackend_OwnershipTemps.cpp).

Serial results: err batch 3.81-3.82 -> 2.96-2.98s (-22%); test_move 1.15-1.16 -> 1.14s.
IR 0/40 vs pre-change capture; test.sh 720/0/8; LSP all pass. All failure exits in
LoadCoreBitcodeIfFresh remain before the context/module swap (re-verified).

Item 10 CLOSED: test_move -o with -v prints "core bitcode cache: hit" - the cache-cold look
of the p_large sample was sampling-resolution artifact, not a bug.

Item 1 (lazy bitcode) in flight; spec scratch/SPEC_lazybc.md includes the mandatory hazard
audit: unmaterialized functions have empty()==true / isDeclaration()==false, so every
"already has a body?" decision must treat isMaterializable() as having a body.

## Batch item 1 - lazy core bitcode - LANDED 2026-08-28 (timebox 2)

getLazyBitcodeModule replaces parseBitcodeFile; MemoryBuffer owned alongside the module
(swap/reset lifecycle). Output compiles (-o/-l/-b/isolated) materialize EAGERLY right after
cache load (frontend may fold core ctors while walking user code); only --check and LSP
Analyze stay lazy end-to-end. MaterializeCoreIfLazy() choke points also guard RunBaselinePasses,
GlobalDCE, AuditIsolatedModule, emit paths. Hazard audit done: every body-presence check
(fn->empty()/isDeclaration()/size()) now treats isMaterializable() as HAVING a body
(definition-skip sites in Aggregates/Declarations/Interfaces); CalleeHasSideEffectsSafe goes
conservative (assume effects) for unmaterialized callees - a discard-hint could diverge
between --check (lazy) and -o (eager) for pure core callees; no test depends on it.

Serial: err batch 2.96-2.98 -> 1.91-1.95s (-35%); test_move 1.14 -> 1.11-1.17s; hello --check
floor 0.03 -> 0.01s. IR 0/40; test.sh 720/0/8; LSP green.

Timebox-2 cumulative: batch 3.81 -> 1.93s (-50%), all four audit items + free deletion landed.

## Timebox 3 - test_lsp.sh + example_mac.sh focus - 2026-08-28

Baselines (macOS arm64, Release, warm --init-local, post-timebox-2 compiler):

| Suite | Wall | CPU | Shape |
|---|---|---|---|
| test_lsp.sh Release | 18.2s | 65s (375%) | 17.6s of it is the 171-file bulk sweep; pool=4 |
| example_mac.sh | 30.8s | 26.6s (92%) | fully SERIAL, 44 cases |

Bulk-sweep profile (sample of the cflat LSP process, pool=4): ANTLR prediction dominates
(closure_ + ATNConfig churn + malloc), i.e. the sweep is parse-bound; ~4.5% DFA-lock
contention (psynch_mutexwait). Avg ~340ms CPU per swept file.

Why per-file cost is that high: ui examples pay ~1s per analysis importing the core ui
library. Time-trace of `gallery.cb --check` (1.06s total): ProcessImports 987ms - Parse of
ui_native.cb / ui_native/cocoa.cb / host.cb / gallery_app.cb totals 638ms, the rest is the
codegen walk of those imports. Two structural reasons:
- The core bitcode cache holds ONLY runtime.cb's transitive closure (CompileCoreOnly
  imports just runtime.cb). ui_native/* are opt-in core libs, re-compiled per compile.
  Auto-adding them to the cache would be a SEMANTIC change (their symbols would resolve
  without the explicit import) - do not do it naively.
- parseTreeCache_ cached core imports only; user imports re-parsed per analysis.

Landed this timebox:
- example_mac.sh parallel job pool (JOBS arg/env, default physical cores): compiles +
  tier-2/3 in the pool, tier-1 GUI selftests still run SERIALLY (Cocoa activation
  flakiness), ordered result aggregation via $OUT/<name>.result files.
  **30.8s -> ~4.9s wall**, 44/0 twice, forced-failure path verified.
- Import parse-tree cache generalized: GetOrParseFile now caches EVERY successfully parsed
  import (not just core) in parseTreeCache_, validated by mtime + file size;
  importedParseStates deleted. Correctness basis: the LSP server has NO unsaved-edit
  overlay for imports (analyzed doc goes via temp file; imports always read from disk), so
  an mtime-validated cache is exactly as fresh as the old re-parse. Only error-free parses
  are cached. (Results below.)

### Timebox 3 results (verified in the main session, warm cache, Release)

| Metric | Before | After |
|---|---|---|
| LSP bulk sweep (171 files, pool 4) | 16.1-19.1s | **9.5-9.7s** (-40%) |
| example_mac.sh | 30.8s | **4.8s** warm (parallel pool; first run after a binary swap is ~11s while compiles re-warm) |
| err batch --check x326 | 1.91-1.95s | 1.83-2.0s (unchanged - err files share few imports) |
| hello --check floor | 0.02s | 0.02s (the extra per-import stat is free) |
| test.sh Release | - | 720 passed, 0 failed, 8 skipped |
| test_lsp.sh Release | - | all pass |

### mimalloc - MEASURED NULL RESULT 2026-08-28

The one-shot compile profile shows malloc/free as the largest top-of-stack bucket
(ANTLR ATNConfig churn). A/B with mimalloc 3.5 via DYLD_INSERT_LIBRARIES (interposition
CONFIRMED active via MIMALLOC_VERBOSE banner): gallery -o 1.33-1.34s both sides, delta
zero. macOS xzone malloc is not the bottleneck; the sample attribution is diffuse churn,
not contention. Do not propose an allocator dependency for compile throughput.

### Remaining one-shot-compile levers, ranked (NOT started)

1. **Per-library bitcode sidecars for opt-in core libs** (ui_native/ui_canvas/...): same
   mechanism as the core cache (bitcode + serialized tables), loaded on `import` via
   llvm::Linker merge + table replay. Kills BOTH the ~638ms parse and the ~340ms codegen
   walk of a ui-example compile (~75% of gallery --check, ~60% of -o). Multi-day,
   structural; the import is still required, so semantics are unchanged - this is the
   correct version of "put ui_native in the core cache".
2. Pre-instantiated common builtin-typed generics in the core cache - still needs the
   example/-tree instantiation census before any design (see the sharing-design section).
3. ANTLR prediction: cheap grammar wins exhausted (see the timebox-1/2 do-not-retry
   lists); what remains is load-bearing ambiguity or accessor-breaking restructures.

## CodeGenOptLevel mapping - LANDED 2026-08-28 (timebox 2, final item)

createTargetMachine at all 4 emit sites (and CreateOptTargetMachine) now passes
CodeGenLevelFor(cOptLevel_): -O0 (the default) -> CodeGenOptLevel::None (FastISel + fast
regalloc, exactly clang -O0), -O1 -> Less, -O2 -> Default, -O3+ -> Aggressive. Previously
every build paid O2-grade SelectionDAG + full regalloc while the IR side ran only baseline
passes. Behavior note for review: default-build executables now get -O0-quality code (faster
compile, slower runtime); -O2/-O3 builds are byte-for-byte unchanged, and performance.bat
already mandates -O2. IR is emitted before isel, so --out-lli output is unchanged (0/40).

Serial: test_move -o 1.14 -> 0.92s (-19%); err batch unchanged ~1.99s (no codegen in --check).
test.sh 720/0/8 (all programs still pass running -O0 code within timeouts); LSP green;
-O2 compile sanity-checked.
