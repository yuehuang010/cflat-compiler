# C++ interop / bridge fix queue

Built 2026-09-27 from the open `internal/issue/` files that concern C++ interop and the CFlat <-> C++
bridge; regrouped 2026-09-28 by fix TYPE and extended with the 12 issues filed during the C++ perf run.
Native-only issues are not in the queue. Workflow is `internal/skill/fix-issue/SKILL.md` (full mode by
default, batch mode only where marked). The main session owns this file: agents never edit it (see the
worktree-agents rule). A landed row moves to LANDED with its hash, and its issue file is deleted in the
fix commit.

## Landing gate (every row)

Green on the rebased branch, run by the main session before `git merge --ff-only`:

```bash
./cmake_build.sh release && x64/Release/cflat --init-local
bash test.sh Release -j 4          # 0 failed
bash test_example.sh               # 0 failed
./test_libs.sh -j 2                # tier 1: 0 FAIL
./test_libs.sh -t 2 -j 1           # tier 2 (eigen): 0 FAIL; an XPASS means a DISABLED marker comes off in the same commit
./test_libs.sh -t 3 -j 1 torch     # torch 11/0 - required for any change under cflat/*CInterop*, *Overloads*, C++ call/ctor/operator paths
```

Resource rule: agents never run the torch tier and run tiers 1-2 at `-j 2` at most (torch compiles
are ~2 GB each). Only the main session runs torch, at `-j 1`, one run at a time.

**Compile-time gate (maintainer, 2026-09-28): no fix may regress test_libs cold or warm time.** Before
each ff-merge the main session runs `scratch/libs_perf.sh <run-id>` on the rebased branch (cheaders
wiped = cold with core warm, then warm; tiers 1-3 at -j 1, plus `scratch/cmp/parity.sh 3`) on a quiet
machine and compares it with the baseline `scratch/libs_perf_master-60effbe4.log`. Baseline: tiers 1-3 31/0, cold sum 170 s (torch 5-7 s per case), warm sum 3 s, parity clang++ 3.26 / warm 0.47 / cold 5.25 s (load ~2-3). A case whose cold or
warm time rises past noise (> 1 s or > 10% on the tier totals, or parity cold/warm median up > 5%) blocks
the merge until explained or fixed; re-measure once before blaming the branch (load moves these numbers).
The new master's log becomes the next baseline. Batched landings measure once per batch. Agents never
run it (torch tier); agent briefs say "a fix that adds clang requests or re-parses on a hot path must
say so in the report".

Every C++ behaviour leg is checked against a `clang++ -std=c++20` oracle program (value AND overload
pick), per the skill. Cache-touching rows (type G) add a cold + warm torch run to the gate.

## How to read the groups

Rows are grouped by the KIND of fix, which decides the mode and the reviewer's focus:

| Type | Kind | Mode | Reviewer focus |
|------|------|------|----------------|
| A | crash / assert / link failure | full | root cause, not the symptom; LogError for any user-reachable assert |
| B | wrong value / wrong overload pick | full | clang oracle on every sibling spelling |
| C | wrongly accepted (adds a rejection) | full | false-refusal sweep: tiers 1-3 + test_cpp_interop* |
| D | wrongly refused (acceptance gap) | batch where marked, else full | new accepts match clang pick + value |
| E | temporary lifetime / leak / dangle | full, opus-tier area | ctor/dtor counts, ASan-style probes |
| F | diagnostic text / surfacing only | batch | message text, `clang: ` prefix rule |
| G | request cache / incremental infra | full | cold + warm + rebuild equivalence |
| P | plans and large staged work | plan | one phase per branch |

A **run** is one worktree + one branch. Issues that share a function go in the same run (never two
concurrent runs on the same function). Run ids are new for this regroup; old ids stay in LANDED.

## In flight

| Run | Branch | Issue(s) | Notes |
|-----|--------|----------|-------|

## A - crashes, asserts, link failures

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| C4 | p3/cpp-trivial-class-operator-assign-int-rejected-as-scalar-store | trivial-class assignment path vs declared operator= | NEW 2026-09-28 (A7) |
| A3 | p3/cpp-incremental-retry-after-failed-parse-crashes | ModuleBuilder / IncrementalAction GenModule | latent (t14-t16, t31 cold retries); known trigger closed. Same subsystem as A1 - run AFTER A1 lands |

## B - wrong value / wrong overload pick

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| B5 | p3/cpp-const-twin-overloads-collapse | C++ class member registration (const / non-const twins share one CFlat signature) | NEW 2026-09-28 (B1 matrix, OUT cells C1/C4, E1-E5, F1/F4, H1/H4/H9, L10) |
| B6 | p3/cpp-global-class-assignment-picks-copy-assign-over-converting-assign | global-destination assignment path | NEW 2026-09-28 (A7 review) |
| B12 | p3/cflat-extern-definition-abi-leftovers | function<> to >16B-struct C fn crossing C: reverse-thunk design (RULING) | OPEN (return-ext part landed 2cd2bb58) |
| B13 | p2/owning-struct-borrowed-deref-and-byvalue-param-double-free | `return *o` through a borrowed pointer: refuse like h->f or move like T t = *o | PARKED (RULING); by-value part landed 059d1f42 |
| B14 | p3/c-flexible-array-member-leftovers | sizeof/alignof/move on flexible members | NEW 2026-09-28 (A6 review 3) |
| B23 | p3/c-anonymous-record-member-flexible-array-not-promoted | C anonymous struct member with a flexible array tail unreachable | NEW 2026-09-28 (A6 r2) |
| B15 | p3/cpp-conversion-operator-receiver-leftovers | rvalue-member arms, const pick, ambiguity | NEW 2026-09-28 (A9b reviews) |
| B16 | p3/cpp-operator-move-operand-leftovers | template U&&, elision, move into const&-only ruling | NEW 2026-09-28 (D8 report) |
| C6 | p3/cpp-ctor-ranking-leftovers | decl-form literal retyping; new T() blocked survivor | NEW 2026-09-29 (C5 review) |
| B18 | p3/cpp-assignment-result-leftovers-after-b9 | extra copies (paren call arms, arr elem init) + bare scalar/nested ternary refusals + 3 unrelated | OPEN (P2 landed 1955bd95) |
| B19 | p3/cpp-unary-operator-leftovers-after-d7 | const/non-const free pair rank, unsigned/ll conversions unbound, native member unary on copy | NEW 2026-09-29 (D7 report) |
| B20 | p2/cpp-const-object-nonconst-member-call-writes-readonly | const receiver leftovers: const fields, const T* local, kind-1 refusal, virtual base, ambiguity | OPEN (main part landed 44d3321c) |
| B21 | p2/alias-return-from-temporary-use-after-free | `U x = f(mk(5))` with alias-return f reads freed temp | NEW 2026-09-29 (B13 review) |
| B22 | p2/byvalue-param-owning-leftovers-after-b13 | closure param return rc138, unary by-value consume rc133, lambda/&w writes, C++ move-only return w | NEW 2026-09-29 (B13 r2) |
| G6 | p3/header-cache-residual-growth | in-version prune age rule (sigbase, dead request configs, cxxdemand) | NEW 2026-09-29 (split from G5) |
| B2 | PARKED 2026-09-28 after 3 Codex rounds (scratch/b2_parked.patch): p3/cpp-string-literal-template-deduction-and-unspellable-char-args + p3/cpp-std-min-long-and-pointer-arguments-refused. Free-array issue closed (premise disproved, [over.ics.rank]/3.2.1). Restart from master on opus with the narrower design in the issue files | RequestCxxFunctionTemplate argument spelling (CxxStringLiteralSpelling, InferImplicitCxxArgumentType) | NEW. One mechanism: spell array / literal lvalues as `*reinterpret_cast<E (*)[N]>(p)` like the member path (e6c60220); give `long` arithmetic, `char*` and pointer-arithmetic rvalues a C++ spelling |
| B2b | p2/l-suffix-incoherent-on-llp64 | literal `L` typing vs C++ `long` identity | NEW 2026-09-28 (Windows). Same family as B2 (long spelling); may need a ruling on what `5L` means on LLP64 - ask before starting |
| B3 | p3/cpp-pointer-argument-prefers-char-pointer-over-converting-ctor + p3/cpp-single-level-pointer-unproven-at-call-argument + p3/cpp-operator-address-of-operand-not-converted | IsProvenPrimitiveSinglePointerArg / CompareUpconvert | old bucket 2b (2a landed as AT). Pointer-is-not-a-number ruling |
| B4 | p3/cpp-ctor-expression-position-const-ref-copies-lvalue | SelectCxxConstructor, expression-position ctor | 2 bugs; run after C1 (same function) |

## C - wrongly accepted (adds a rejection)

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| C2 | p3/cpp-class-vs-class-template-same-name-conflict-not-diagnosed | CheckCxxNamespaceConflicts | one site |

## D - wrongly refused (acceptance gaps)

| Run | Issue(s) | Site | Mode |
|-----|----------|------|------|
| D1b | p3/cpp-nullptr-t-free-function-and-void-pointer-gaps + p3/cpp-unguarded-header-class-template-lookup-fails-cold (cold-cache only) | free-function selection; incremental request units | NEW 2026-09-28 (D1 leftovers) |
| D5 | p3/cpp-operator-chain-reference-result-refused | fold carry (after D2) | NEW 2026-09-28 (D2 review); batch candidate |
| D6 | p3/cpp-pointer-return-unrequested-specialization-not-retried | refused-member retry eligibility | NEW 2026-09-28 (D3) |
| D4 | p3/cpp-no-unique-address-with-bitfields-or-anonymous-member-refused | PackBitfields | full (AP landed, unblocked) |
| - | p3/cpp-converting-ctor-raw-array-source-refused-implicit, p3/cpp-expression-template-to-cflat-byvalue-class-param-refused (NEW) | converting-ctor path | absorbed by P1 (converting constructors); land there |

## E - temporary lifetime, leak, dangle (one issue per run, opus-tier area)

| Run | Issue | Notes |
|-----|-------|-------|
| E1b | p3/cpp-brace-argument-backing-remaining-shapes (4 open shapes after 86ccc218) | RequestCxxFunctionTemplate, convertElements, ctor-template packs | follow-up; item 1 (function template list reads garbage) is the user-visible one |
| E2 | p3/coalesce-arm-temp-cxx-borrow-and-return-flush-gaps | leak + early free, 3 sub-bugs near DropRetainedJoinArmPtrTemps |
| E3 | p3/cpp-unwind-cleanup-gaps-ctor-new-temporaries | follow-up list after d59df39b / cpp-unwind2 |
| E4 | p3/cpp-new-remaining-allocator-gaps | follow-up list after cpp-class-operator-new (5 items) |

## F - diagnostics only

| Run | Issue(s) | Mode |
|-----|----------|------|
| F1 | p4/localize-relayed-clang-diagnostics + p3/cpp-sink-refusal-cause-lost-under-harvest-suppression | one run: every relayed clang cause surfaces, prefixed `clang: ` (2026-09-27 ruling, no translation). Batch for the prefix sites; the harvest-suppression half is full-mode review |

## G - request cache and incremental infrastructure

| Run | Issue(s) | Mode | Notes |
|-----|----------|------|-------|
| G2 | p3/cpp-wrapper-request-group-omits-type-dependencies + p3/cpp-candidate-tier-differs-cold-vs-warm | batch | RequestGeneratedCxxWrapperUncached, CandidateCxxGroupsFor. After G1 |
| G3 | p3/cpp-check-mode-cannot-repopulate-type-request-cache | full | design: --check writing the cache |
| G4 | p3/cpp-noinc-mode-std-map-lookup-fails | full | root cause not established; investigation first |

## H - coverage only (no known defect)

| Run | Issue | Notes |
|-----|-------|-------|
| H1 | expression-template-ctor half of p3/cpp-inherited-members-and-expr-ctor-coverage-gaps | `requires` / `enable_if` ctor templates, copy/move/dtor counts, by-value `operator=`, same-named records in two namespaces. Cheap; delete the issue when A2 + H1 both landed |

## P - plans and staged work

| Id | Item | Notes |
|----|------|-------|
| P0 | p1/cpp-import-compile-time-parity-with-clang | master: warm 0.15x (met), cold 1.6x plain / 1.2x PGO+ThinLTO. Remaining cold cost is the eager define passes (0.75 s) = ruling item (demand-only bindable surface) + chunk-0 parse; see memory cpp-header-cost-model. Not in the fix timebox unless ruled |
| P1 | plan converting-constructors (`internal/plan/converting-constructors.md`) | ruled 2026-09-26; eigen_05; absorbs D-row converting-ctor-raw-array + expression-template-to-cflat-byvalue-param. Largest item, own staging |
| P2 | plan cflat-struct-nontrivial-cxx-fields (`internal/plan/cflat-struct-nontrivial-cxx-fields.md`) | ruled A 2026-09-27. Phase 0 (stopgap refusal) is small and can go early; phases 1-4 one per branch. Absorbs p2/cpp-struct-list-field-relocated-bitwise, p3/cpp-prvalue-to-cflat-byvalue-param-extra-copy, p3/cpp-typed-local-extra-copy-from-param-and-ctor-arms |
| P3 | std library coverage: cppinterop/stream-classes-no-callable-destructor, cppinterop/stream-open-instantiation-error, cppinterop/std-header-coverage-spike | re-run the survey after B-E land to refresh the gap list |
| P5 | p3/debug-assert-setvisibility-cross-target-elf | cross-target (-p win64) only; fold into p3/cross-target-compile-gaps work |
| P4 | virtual-base layout (item 2 of p3/cpp-inherited-member-access-and-virtual-base-diagnostics) | refused today ("layout cannot be reproduced", CInterop ~14229); needs a plan |

## Parked - needs a maintainer ruling before any work

| Issue | Question |
|-------|----------|
| p4/string-functional-construction-spelling | p4: spelling ruling needed |
| p3/cpp-constructor-call-accepts-implicit-narrowing | scalar conversion table at C++ call arguments (ctor AND free function): master's free path refuses widening like int -> double / short -> double / bool -> double for C++ callees while the 2026-09-04 ruling refuses only narrowing; ctor and free differ in 80/160 cells (scratch/c1_matrix_r3.md). Rule: accept widening everywhere, or keep exact-group only? |
| p3/cpp-unscoped-enum-to-double-param-refused | joins the scalar conversion table ruling above: CFlat refuses int -> double at every call argument (native too), so enum -> double would be a new rule; also `P4(0)` (C++ ambiguous int -> long vs pointer) (D1, 2026-09-28) |
| p2/l-suffix-incoherent-on-llp64 | what `5L` is on LLP64 (i64 vs C++ `long` = 32-bit) |
| P0 remaining cold cost | eager define passes: keep eager or rule a demand-only bindable surface |

## Conflict map (never concurrent)

B3, C1, B4, D1 all touch LLVMBackend_Overloads ctor/argument selection - run them one after another
(order C1 -> B4 -> B3 -> D1, or D1 first as the cheap batch). D2 and D3 share operator dispatch. A1 and
A3 share the incremental action. G1 -> G2 -> G3 share the request-cache key. E1 touches
RequestCxxBraceConstructor, which B2's template spelling calls into - not concurrent with B2.

## Timebox plan (12 h work + 1 h cool-off)

Caps: 3 codex + 2 claude agents, at most 3 implementers; the 2 extra slots are reviewers / advisors.
Codex Luna is default (spiked CODEX_OK 2026-09-28). Torch tier only in the main session, -j 1. Fable
advisor at 3 review rounds. Hours are from the box start.

| Window | Lane 1 (Overloads / ctor) | Lane 2 (templates / operators / lifetime) | Lane 3 (infra / records / crash) |
|--------|---------------------------|-------------------------------------------|----------------------------------|
| h0 - h3 | C1 (rejections, full) | B2 (template arg spelling, full) | A1 (PushDeclContext; start the Debug build at h0) |
| h2 - h5 | B4 then D1 (batch) | D2 (operators) | G1 (cache atomicity; main session runs the torch loop) |
| h4 - h8 | B3 (pointer proof 2b) | B1 (const internally, opus-tier) | A2 (inherited members) |
| h7 - h10 | P2 phase 0 or F1 | D3 (eigen_04) or E1 | A3 or G2 (batch) or C2 |
| h10 - h12 | LAND ONLY: no new starts; finish review rounds, rebase, gate, ff-merge | | |
| h12 - h13 | COOL-OFF: `buildci.sh --nightly` on final master, remove worktrees, Queue LANDED rows, resume notes, memory | | |

Prep done 2026-09-28: main x64/Release rebuilt at master 60effbe4; Codex spike OK; wave-1 briefs
scratch/briefs/fixbox_common.md + fix_c1.md / fix_b2.md / fix_a1.md; launcher
`scratch/briefs/launch_fix.sh <run> <branch>` (c1 fix/ctor-reject, b2 fix/tpl-arg-spell, a1
fix/late-body-ctx); compile-time baseline scratch/libs_perf_master-60effbe4.log. Budget per landing
~20-25 min of main-session time (gate + libs_perf), so land in batches, not one by one.

Order within a lane is a default, not a contract: a lane that frees early pulls the next unblocked
row from the same lane, then from H1 / C2 / F1 (cheap fillers). Rows that do not start move to the next
box untouched. Main-session review cadence: batch ready branches into one reviewer round
(feedback-batch-reviews-across-branches), one landing gate per batch.

## LANDED

| Hash | Row | Issue(s) |
|------|-----|----------|
| 44d3321c | B20 | p2/cpp-const-object-nonconst-member-call-writes-readonly (main) | const twin overloads for const receivers (methods, binary + unary member ops), refusal for non-const-only named calls |
| 1955bd95 | B18 | p3/cpp-assignment-result-leftovers-after-b9 (P2) | parenthesized ternary of assignment arms takes the ternary route (return + decl) |
| 059d1f42 | B13 | p2/owning-struct-borrowed-deref-and-byvalue-param-double-free (by-value part) | by-value owning param return/whole-write -> consume-inferred sink or copy; binary op operand consume; leftovers B21, B22 |
| e17348f5 | B17 | p3/cpp-ulong-literal-argument-ambiguous-with-int-overload | suffixed literal [lex.icon] identity for C++ candidates only; native keeps lowered name |
| 10b49824 | D7+C3 | p3/cpp-free-unary-operator-on-class-unsupported, p3/cpp-unary-plus-on-class-without-operator-accepted | free/template unary operator lookup, one-conversion built-in step, unary + refusal, own-slot receiver (read-only keeps copy); leftovers B19, B20 |
| 1955bd95 | B9 | p3/cpp-assignment-result-by-value-leftovers | assignment-result consumers (decl, return, ternary, statement temp) match clang counts; paren-ternary leftover = B18 |
| e17348f5 | C5 | p3/cpp-ctor-default-argument-ambiguity-not-refused | ctor ranking reuses per-argument conversion-category ranking; indistinguishable pairs refused like clang |
| e17348f5 | B8 | p3/cpp-subscript-operator-overload-picks-last-declared | operator[] index carries its integer identity; native member ops of other types filtered for C++ receivers |
| 059d1f42 | A10 | p2/native-owning-struct-init-from-compound-assign-traps | native by-value compound result adopts destination provenance (implicit move) + moved-receiver compound read refused |
| 2cd2bb58 | B10 | p3/c-function-pointer-indirect-call-narrow-int-not-extended | indirect C prototype call: param signext/zeroext per target (enum = backing); extern def keeps RETURN ext (B12 part) |
| 059d1f42 | B11 | p2/brace-init-double-field-stores-float | brace-init numeric field uses ConvertScalarToType like `=` |
| 10b49824 | D8 | p2/cpp-operator-move-operand-refused-for-nontrivial-class | `move d` into T&&/by-value operator operand binds in place like f(move d); leftovers B16 (incl. ruling) |
| d464d9b6 | G5 | p3/header-cache-old-version-dirs-never-removed | first header import prunes v<M> dirs older than 7 days (no symlinks, exact names); residual growth -> G6 |
| 2cd2bb58 | B7 | p2/test-c-interop-exits-5-at-o2 | extern C narrow-int signext/zeroext per target; arm64 >16B struct caller-copy pointer (in place); test_c_interop at -O2; leftovers B10, B12 |
| 10b49824 | A9b | p2/cpp-operator-bool-called-on-bitwise-copy | conversion operators take the loaded object's storage (incl. nested ternary joins, main-session round 3); leftovers B15 |
| 2cd2bb58 | A6 | p2/c-flexible-array-member-bound-as-pointer | flexible/zero-length member decays to T* in value contexts; sizeof/whole store refused; array ++/-- refused (main-session fix in place); leftovers p3/c-flexible-array-member-leftovers |
| d464d9b6 | A8 | p2/cpp-free-operators-same-record-duplicate-strong-definition | request cache group stamp = include-closure content hash; .rq line-3 scoped prune; warm/cold A/B even; leftovers in G5 + cross-target issue |
| 10b49824 | D3 | p3/cpp-operator-returning-unrequested-specialization-not-bound | on-use request of unrequested spec returns (binary, compound, postfix); explicit move operand binds the temp; leftovers D8 + D6 appendix |
| c295d7ef | A9 | p2/cpp-class-declaration-init-from-assignment-leaves-object-unconstructed | assignment result as initializer / return copies from the destination; by-value operators construct in place; leftovers B9 |
| e17348f5 | D1 | p3/cpp-ctor-nullptr-t-param-refused, p3/cpp-nonconst-default-refused-when-an-argument-is-a-string-literal | nullptr_t ctor param (value/const&/&&), ctor ranking exact-before-defaults, dflt wrapper spelling; enum->double dropped (ruling), scoped-enum item rewritten to unguarded-header issue |
| c295d7ef | A7 | p2/compiler-segfault-cxx-class-assignment-as-logical-operand | class assignment as ||/&&/!/?:/while operand: destination lvalue, operator bool, refusal like clang |
| 2c6caaac | A45 | p2/debug-assert-getterminator-in-try-direct-cxx-assign-operator, p2/debug-assert-irmover-mapping-to-source-type-in-companion-link (Debug sweep aborts 3 -> 1: setvisibility P5 left) |
| 2c6caaac | E1 | p3/cpp-brace-argument-backing-remaining-shapes items 1-3 of the old list (class-typed params, inherited ctors, RequestCxxBraceFunction; loop/ternary placement); file kept with 4 open shapes (function templates, convertElements, param packs, list-ctor templates) |
| 2c6caaac | C1 | p3/cpp-ambiguous-default-ctor-picks-nullary (every default-construction spelling), p3/cpp-trivially-copyable-private-default-ctor-silently-zeroed; p3 narrowing kept, parked for the scalar-conversion ruling |
| d464d9b6 | G1 | p2/cpp-libs-cache-intermittent-alias-miss-after-rebuild (20/20 torch under load), p3/cpp-request-cache-not-keyed-on-compiler-build (CFLAT_CACHE_BUILD_STAMP=1 opt-in, default off until cold <= 1.1x clang), p2/cpp-demand-replay-undeclared-cflat-user (cache 129); filed p3/cpp-default-wrapper-batch-chunk-stores-stripped-prefix |
| e17348f5 | B1 | p3/cpp-const-pointee-lost-in-member-template-ranking, p3/cpp-assignment-through-const-reference-result-accepted (cache 128); filed p3/cpp-const-twin-overloads-collapse |
| 8a11ef39 | D2 | p3/cpp-pointer-left-free-operator-not-dispatched, p3/cpp-chained-string-operator-fold-refused, p3/cpp-free-compound-assign-operator-template-class-operand-refused |
| 2c6caaac | A2 | p2/cpp-inherited-static-data-member-link-failure (root cause: out-of-line inline + template statics), private-base diagnostic, unary operator hiding; coverage legs |
| 2c6caaac | A1 | p2/cpp-late-template-body-pushdeclcontext-assert (+ params-twice assert fixed in place) |
| - | B2 | p3/cpp-free-template-array-arg-skipped-for-decayed-exact CLOSED, premise disproved |
| 77a7134b | AP | p2/c-bitfield-union-access-and-enum-bitfield (cache 114) |
| e6c60220 | AQ | p2/cpp-member-template-vs-nontemplate-overload-ranking (cache 115) |
| be21a0f8 | AV | p3/cpp-free-wrapper-returns-reference-into-own-frame |
| 3b3df77d | AY | p2/eigen07-warm-cache-inline-asm-fatal (untracked issue, deleted) |
| 3c0b5e78 | AT | bucket 2a: 5 pointer-argument issues (p2 x4, p3 template-bool) |
| 37f2c948 | PB | p1 parity: by-value gate projects only dtor + copy/move ctors (393 -> 111 request chunks, cold -1.05 s, warm -0.36 s) |
| 20101f74 | PP | p1 parity: macro prepass folded into the chunk-0 parse, synthetic closers for an unbalanced header (cold -0.54 s) |
| 53ac9fef | PE | p1 parity: definition-emission walks trimmed (error sweep gated, static-member walk skips functions; -25 ms, trace scopes) |
| bd1def8e | PF | p1 parity: callback ABI plans project only the by-value slice of their records (-0.04 s, 62 fewer request files) |
| 72e9b223 | AZ | p2/cpp-free-template-operator-std-function-temporary-bitwise-copy (untracked, deleted) |
| d4fa2eaa | AM | p3/cpp-inherited-static-members-and-member-operators-not-found |
| 0d1386fa | AU | p3/cpp-variadic-ctor-wrapper-from-expression-template-not-registered (untracked, deleted); eigen_03/04/05 enabled, tier 2 20/0/0 |
| d9da1ee6 | BA | p2/cpp-template-wrapper-string-literal-const-ref-returns-wrapper-local (untracked, deleted) |
| 7ecfac3f | AX | p3/cpp-ctor-thunk-brace-argument-backing-array-dangles (Fable-advised; remaining shapes -> p3/cpp-brace-argument-backing-remaining-shapes) |
| 82370cc5 | AS+AW+BD | p1 parity: P1+P2+P3 (lazy std::function binds, demand-driven companion per group, cache 117); torch warm 0.9x, cold 3.5x |
| 66d795b2 | BF | move dataflow RPO worklist: test_cpp_interop main 163 s -> 0.27 s; test.sh ~279 -> ~126-159 s |
| 4920585a | BH | C++ import: skip unneeded inline bodies in initial group parse, late-parse demanded ones (group parse 3.88 -> 2.43 s, torch cold 11.5 -> 10.4 s); header cache v119 |
| 2a0a0d58 | DI | C++ default-wrapper drop: name index built once instead of O(n^2) rescan (torch cold -0.7 s) |
| bda8b836 | BJ | C++ import cold glue: namespace scan once per header; C header disk cache written on a joined worker thread (torch cold ~-0.5-1 s) |
| e95d8a0c | BK | C++ type requests: free-operator candidate index per header root, owned by the group (was whole-TU walk per request) |
| 7b1755eb | BG | C++ import: lazy record projection (shells, layout on demand, exact member projection) + [over.ics.rank] 3.2.3 T&& vs const T& unblock; torch warm 3.26 -> 1.33 s (0.4x), cold 10.0 -> 8.0 s |
| 02ae6de0 | CJ | C header disk cache: read joins only the pending write of its own entry (torch cold -0.44 s) |
| 2aa9ed2e | BL | C++ harvest visitor pruning (DeclVisitor skips bodies/TypeLocs; ErrorBodySweep no double descent) |
| e8bd177b | BN | C++ by-value gate decides from raw cached record data; pending by-value layouts bind on first use (torch cold -0.35 s) |
| 92149b7f | BQ | C++ reverse spelling index no longer rebuilt after every member projection; SqueezeCxxSpelling no-whitespace fast path (torch warm ~1.5 -> ~0.85 s) |
| 56311538 | DS | Dependency recording: memoized canonical directory + listing, one lstat per file; manifest reuses recorded paths (+ 649c8ba3 case-insensitive leaf fix) (torch warm ~-0.3 s) |
| 6f221fce | UV | C++ definitions plan: UsedStaticVarVisitor skips statements and TypeLocs |
| edc89a47 | BP | C++ warm-edit demand replay (cache v124); linker output held back after replay, failed link -> cold retry (torch warm-edit 7.6 -> 6.0 s) |
| 85dfb8cc | NS | Header namespace scan: bulk read + up to 4 workers (cold ~-0.1 s) |
