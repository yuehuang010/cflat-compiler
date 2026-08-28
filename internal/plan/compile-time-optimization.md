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
