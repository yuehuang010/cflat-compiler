# C++ interop / bridge fix queue

Built 2026-09-27 from the open `internal/issue/` files that concern C++ interop and the CFlat <-> C++
bridge; regrouped 2026-09-28 by fix TYPE and extended with the 12 issues filed during the C++ perf run.
Native-only issues are not in the queue. Workflow is `internal/skill/fix-issue/SKILL.md` (full mode by
default, batch mode only where marked). The main session owns this file: agents never edit it (see the
worktree-agents rule). A landed row is removed (git log carries its hash), and its issue file is deleted in the
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
concurrent runs on the same function). Run ids are new for this regroup; old ids are retired.

## In flight

| Run | Branch | Issue(s) | Notes |
|-----|--------|----------|-------|


## A - crashes, asserts, link failures

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| A3 | p3/cpp-incremental-retry-after-failed-parse-crashes | ModuleBuilder / IncrementalAction GenModule | latent (t14-t16, t31 cold retries); known trigger closed. Same subsystem as A1 - run AFTER A1 lands |

## B - wrong value / wrong overload pick

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| B5 | p3/cpp-const-twin-overloads-collapse | C++ class member registration (const / non-const twins share one CFlat signature) | NEW 2026-09-28 (B1 matrix, OUT cells C1/C4, E1-E5, F1/F4, H1/H4/H9, L10) |
| B12 | p3/cflat-extern-definition-abi-leftovers | function<> to >16B-struct C fn crossing C: reverse-thunk design (RULING) | OPEN (return-ext part landed 2cd2bb58) |
| B13 | p2/owning-struct-borrowed-deref-and-byvalue-param-double-free | `return *o` through a borrowed pointer: refuse like h->f or move like T t = *o; `return *this` refusal RULED 2026-10-01 | PARKED (RULING); by-value part landed 059d1f42 |
| B14 | p3/c-flexible-array-member-leftovers | move a->data (RULING: explicit move of a raw pointer) + 3 pre-existing P3 | OPEN (sizeof/alignof landed 118c2769) |
| B16 | p3/cpp-operator-move-operand-leftovers | template U&&, elision, move into const&-only ruling | NEW 2026-09-28 (D8 report) |
| B18 | p3/cpp-assignment-result-leftovers-after-b9 | extra copies (paren call arms, arr elem init) + bare scalar/nested ternary refusals + 3 unrelated | OPEN (P2 landed 1955bd95) |
| B21b | p2/alias-return-byvalue-param-leftovers-after-b21 | named alias into sink, array-element store, chained decl consume, C-linkage definitions, prototype link error, stale shallow copy | W12 landed mixed named+temp + fn-value message 2026-09-30; rest open |
| W13b | p3/cpp-overload-variadic-and-conversion-gaps | variadic-set ambiguity, defaulted param before ellipsis, member variadics, user conversions at C++ call args, inline variadic link, non-record const pointers | NEW 2026-09-30 (W13 reviews) |
| G6 | p3/header-cache-residual-growth | in-version prune age rule (sigbase, dead request configs, cxxdemand) | NEW 2026-09-29 (split from G5) |
| B2 | PARKED 2026-09-28 after 3 Codex rounds (scratch/b2_parked.patch): p3/cpp-string-literal-template-deduction-and-unspellable-char-args + p3/cpp-std-min-long-and-pointer-arguments-refused. Free-array issue closed (premise disproved, [over.ics.rank]/3.2.1). Restart from master on opus with the narrower design in the issue files | RequestCxxFunctionTemplate argument spelling (CxxStringLiteralSpelling, InferImplicitCxxArgumentType) | NEW. One mechanism: spell array / literal lvalues as `*reinterpret_cast<E (*)[N]>(p)` like the member path (e6c60220); give `long` arithmetic, `char*` and pointer-arithmetic rvalues a C++ spelling |
| B2b | p2/l-suffix-incoherent-on-llp64 | literal `L` typing vs C++ `long` identity | NEW 2026-09-28 (Windows). Same family as B2 (long spelling); may need a ruling on what `5L` means on LLP64 - ask before starting |
| B3 | p3/cpp-pointer-argument-prefers-char-pointer-over-converting-ctor + p3/cpp-single-level-pointer-unproven-at-call-argument + p3/cpp-operator-address-of-operand-not-converted | IsProvenPrimitiveSinglePointerArg / CompareUpconvert | old bucket 2b (2a landed as AT). Pointer-is-not-a-number ruling |

## C - wrongly accepted (adds a rejection)

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|

## D - wrongly refused (acceptance gaps)

| Run | Issue(s) | Site | Mode |
|-----|----------|------|------|
| D1b | p3/cpp-nullptr-t-free-function-and-void-pointer-gaps + p3/cpp-unguarded-header-class-template-lookup-fails-cold (cold-cache only) | free-function selection; incremental request units | NEW 2026-09-28 (D1 leftovers) |
| - | p3/cpp-converting-ctor-raw-array-source-refused-implicit, p3/cpp-expression-template-to-cflat-byvalue-class-param-refused (NEW) | converting-ctor path | absorbed by P1 (converting constructors); land there |

## E - temporary lifetime, leak, dangle (one issue per run, opus-tier area)

| Run | Issue | Notes |
|-----|-------|-------|
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
| P0 | p1/cpp-import-compile-time-parity-with-clang | master 93b361f6 (perf timebox 2026-09-29c, PGO LLVM): test_libs geomean cold 1.14x (was 1.75x), warm 0.24x; worst fmt_03 1.38x, json_01 1.31x, simdjson_02 1.20x, torch 1.02-1.20x. Rulings R1/R3/R4 landed. Next: fmt_03 / simdjson_02 residual (profile scratch/repro_keep/t1/t1_profile.md), json stage-2 remainder, N28 / N35 costs; resume notes scratch/resume_2026-09-29c.md |
| P1 | plan converting-constructors (`internal/plan/converting-constructors.md`) | ruled 2026-09-26; eigen_05; absorbs D-row converting-ctor-raw-array + expression-template-to-cflat-byvalue-param. Largest item, own staging |
| P2 | plan cflat-struct-nontrivial-cxx-fields (`internal/plan/cflat-struct-nontrivial-cxx-fields.md`) | ruled A 2026-09-27. Phase 0 (stopgap refusal) is small and can go early; phases 1-4 one per branch. Absorbs p2/cpp-struct-list-field-relocated-bitwise, p3/cpp-prvalue-to-cflat-byvalue-param-extra-copy, p3/cpp-typed-local-extra-copy-from-param-and-ctor-arms |
| P3 | p3/cpp-std-header-survey-refresh | re-run the std header L0/L1 survey on macOS and Windows to refresh the gap list; untouched headers need a CFlat-surface ruling |
| P5 | p3/debug-assert-setvisibility-cross-target-elf | cross-target (-p win64) only; fold into p3/cross-target-compile-gaps work |
| P4 | virtual-base layout (item 2 of p3/cpp-inherited-member-access-and-virtual-base-diagnostics) | refused today ("layout cannot be reproduced", CInterop ~14229); needs a plan |

## New or rewritten 2026-09-29 (leftovers from landed rows; group at the next queue build)

| Run | Issue | Summary | Status |
|---|---|---|---|
| N1 | p2/byvalue-param-owning-leftovers-after-b13 | By-value owning param leftovers after B13 | NEW 2026-09-29 |
| N2 | p2/cpp-arm-temp-stored-then-throw-freed-on-unwind | [P2] C++ arm temp stored then throw is freed during unwind | NEW 2026-09-29 |
| N3 | p2/cpp-const-object-nonconst-member-call-writes-readonly | loop back-edge const-pointer dataflow, virtual-base twin | W13 landed const auto copy, const globals, const-pointer overload selection 2026-09-30; rest open |
| N4 | p2/cpp-conversion-operator-multiword-silent-mispick | C++ multi-word conversion operators (`operator unsigned`, `long long`, `signed char`, `long double`) silently  | NEW 2026-09-29 |
| N5 | p2/cpp-conversion-operator-on-destroyed-ternary-temporary | C++ conversion operator runs after ternary temporary destruction | NEW 2026-09-29 |
| N6 | p2/cpp-explicit-specialization-separate-import-gets-primary-layout | Explicit specialization in a separately imported header gets the primary template's layout | NEW 2026-09-29 |
| N7 | p2/o2-builtin-folding-overrides-cflat-definitions | test_cpp_interop.cb still fails at -O2 (non-unwind leftovers) | NEW 2026-09-29 |
| N8 | p3/coalesce-nested-ternary-arm-new-verifier-failure | [P3] Coalesce containing nested ternary arm new fails verification | NEW 2026-09-29 |
| N9 | p3/cpp-assign-operator-route-leftovers | C++ assignment operator route leftovers | NEW 2026-09-29 |
| N10 | p3/cpp-brace-argument-backing-remaining-shapes | Brace-list backing: optional constructor parameter case remains | NEW 2026-09-29 |
| N11 | p3/cpp-constructor-call-accepts-implicit-narrowing | Bucket: C (ruling needed: scalar conversion table at C++ calls) | NEW 2026-09-29 |
| N12 | p3/cpp-conversion-operator-receiver-leftovers | C++ conversion-operator receiver and class-result leftovers | NEW 2026-09-29 |
| N13 | p3/cpp-ctor-expression-position-const-ref-copies-lvalue | C++ ctor in expression position - remaining `const T&` copies and enum `T&&` refusal | NEW 2026-09-29 |
| N14 | p3/cpp-ctor-ranking-leftovers | P3 C++ constructor ranking leftovers after C6 round 2 | NEW 2026-09-29 |
| N15 | p3/cpp-default-wrapper-batch-chunk-stores-stripped-prefix | C++ header default-wrapper batch chunk stores a stripped prefix (latent replay failure) | NEW 2026-09-29 |
| N16 | p3/cpp-float-ctor-double-verifier | P3 | NEW 2026-09-29 |
| N17 | p3/cpp-namespace-conflict-check-leftovers | C++ namespace conflict check leftovers (after C2) | NEW 2026-09-29 |
| N18 | p3/cpp-pointer-return-unrequested-specialization-not-retried | Remaining C++ free-operator and converting-constructor retry gaps | NEW 2026-09-29 |
| N19 | p3/cpp-private-base-instance-member-diagnostics | Instance members and operators behind a private/protected base: generic refusal text | NEW 2026-09-29 |
| N20 | p3/cpp-reference-returning-shift-and-logical-operators-refused | Bucket: p3 (C++ interop operators; found by the D5 round-2 review, 2026-09-29) | NEW 2026-09-29 |
| N21 | p3/cpp-static-data-member-use-forms | C++ static data members: chained field access and instance-receiver access refused | NEW 2026-09-29 |
| N22 | p3/cpp-template-ctor-deduction-enum-and-arith-rvalue | C++ template constructor deduction - enum lvalue and arithmetic rvalue deduce the wrong T | NEW 2026-09-29 |
| N23 | p3/cpp-unary-operator-leftovers-after-d7 | Unary operator leftovers after D7+C3 | NEW 2026-09-29 |
| N24 | p3/cpp-unrequested-specialization-pointer-eager-request | Bucket: p3 (C++ interop compile time; left by D6, fix/unrequested-spec-pointer-retry, 2026-09-29) | NEW 2026-09-29 |
| N25 | p3/pointer-to-number-diagnostic-hint-leftovers | Pointer -> number store refusal: diagnostic hint leftovers | NEW 2026-09-29 |
| N26 | p3/return-keep-rule-later-defined-callee-leaks | Bucket: p3 (ownership temporaries; left by E2, fix/coalesce-arm-temp-gaps, 2026-09-29) | NEW 2026-09-29 |
| N27 | p3/cpp-default-wrapper-tie-list-shows-wrapper-shape | Ambiguous call through a default-argument wrapper lists wrapper shapes, not declarations (R3 review) | NEW 2026-09-29 perf |
| N28 | p3/cpp-deferred-special-members-mid-size-harvest-cost | R1 deferral costs +1-2% cold on 64..~500-record harvests (interop fixtures); cutoff adjustable later (maintainer) | NEW 2026-09-29 perf |
| N29 | p2/cpp-copy-of-class-with-ill-formed-copy-ctor-compiles | Copy of a C++ class whose implicit copy ctor is ill-formed compiles; now exit 139 in clang CodeGen, implicit or defaulted (pre-existing, R1 + D8 reviews) | NEW 2026-09-29 perf |
| N30 | p2/cpp-import-transitive-syntax-error-hangs | Syntax error in a transitively included C++ header hangs the compile (pre-existing, H1 review) | NEW 2026-09-29 perf |
| N31 | p3/cpp-interpreter-drops-cc1-only-flags | -Xclang cc1-only flags silently dropped by the Interpreter driver (pre-existing, H1 review) | NEW 2026-09-29 perf |
| N32b | p3/cpp-ctor-refusal-text-gaps-after-w11 | default-member-init refusal lacks clang: prefix, ctor template names wrong overload (W11 landed the body-failure fix 2026-09-30) | NEW 2026-09-30 |
| N33 | p3/cpp-invalid-virtual-body-link-failure | Invalid virtual body of a C++ class template = link failure, not a use-site error (pre-existing, R4 review) | NEW 2026-09-29 perf |
| N34 | p3/cpp-deployment-target-env-core-cache-miss | Non-default MACOSX_DEPLOYMENT_TARGET misses the core bitcode cache every compile (R4 review) | NEW 2026-09-29 perf |
| N35 | p3/cpp-demand-bodies-torch-cold-cost | R4 costs +4.4% cold instructions on torch (json -28%); verdict writes batched in 93b361f6 (-0.55%), rest is clang instantiation | PART 2026-09-29d |
| N36 | p3/cpp-demand-second-use-of-failed-helper-generic-text | Second use through an already-failed helper refused without clang's text (R4 review) | NEW 2026-09-29 perf |
| N37 | p3/cpp-signature-registration-projects-records | Registration projects every record a signature names; -3.6% torch only by skipping projection, which breaks overload order (D4) | NEW 2026-09-30 perf |
| N38 | p2/cpp-static-inline-member-failed-initializer-reads-zero | Static inline member whose initializer fails static_assert reads 0 silently (pre-existing, D8 review) | NEW 2026-09-30 perf |

## std smoke suite gaps 2026-10-01 (test_libs/std DISABLED cases; group at the next queue build)

Fixing a row removes the DISABLED marker of its case(s) in the same commit (`./test_libs.sh std --include-disabled` shows XPASS).

| Run | Issue | DISABLED case | Summary |
|---|---|---|---|
| S1 | p2/std-cflat-lambda-to-std-function | std_11_92 | lambda cannot initialize std::function (no signature propagation); also blocks std.visit with a lambda |
| S2 | p2/cpp-cflat-callable-to-deduced-template-param | std_20_93, std_20_97 | CFlat function<>/lambda/function name refused as a deduced C++ callable (erase_if, jthread); std.function wrapper works |
| S3 | p2/cpp-ranges-view-callable-argument-unspellable | std_20_94 | views.filter/transform closure return type (__pipeable) not bindable |
| S4 | p2/std-is-null-pointer-trait | std_14_93 | is_null_pointer<nullptr_t>::value reads false (wrong value) |
| S5 | p2/std-utility-move-cannot-bind | std_11_94 | KEEP std::move (RULED 2026-10-01): T&& C++ call result as init/assign/return move source + specific diagnostics (std.move on a CFlat value -> suggest `move x`) |
| S6 | p2/std-tie-cannot-bind | std_11_93 | std.tie wrapper cannot be registered |
| S7 | p2/std-unique-ptr-custom-deleter | std_11_95 | unique_ptr<T, D> with a function-type deleter: invalid specialization |
| S8 | p2/std-enable-shared-from-this-base | std_11_96 | CRTP base std.enable_shared_from_this<T> on a CFlat struct not a C++ class |
| S9 | p2/std-chrono-duration-cast | std_11_91 | duration_cast<seconds>(...) parsed as a class type |
| S10 | p2/std-byte-interop | std_17_92 | std.byte (enum class) cannot be named |
| S11 | p2/std-not-fn-return-type | std_17_93 | not_fn return type (__not_fn_t) not bindable |
| S12 | p2/std-charconv-char-buffer | std_17_94 | to_chars/from_chars: no matching overload for a char buffer |
| S13 | p2/std-filesystem-directory-iterator | std_17_96 | directory_iterator not iterable from CFlat (bound to list<string> overload) |
| S14 | p2/cpp-std-barrier-type-unavailable | std_20_96 | std.barrier (defaulted template arg) not found; `std.barrier<>` is a parse error |
| S15 | p3/std-make-unique-array | std_14_91, std_20_92 | T[] template argument (make_unique<int[]>, make_shared<int[]>) has no spelling |
| S16 | p3/std-index-sequence-size | std_14_92 | integer_sequence::size() static member unknown |
| S17 | p3/std-filesystem-path-join | std_17_95 | path operator/ not found |
| S18 | p3/cpp-namespace-inline-constexpr-variable-lookup | std_20_91 | std.numbers.pi (namespace inline constexpr variable) not found |
| S19 | p3/cpp-std-format-to-n-unreachable | std_20_95 | format_to_n not bound (clang: no member) |
| S20 | p3/cpp-static-data-member-use-forms (existing, appended) | std_20_98 | std.endian.native enumerator/static member not resolved |

## Ruled 2026-10-01 - ready to schedule (rulings recorded in each issue file)

| Ruling | Issue | Work |
|---|---|---|
| Q1 | p2/owning-struct-borrowed-deref-and-byvalue-param-double-free | refuse non-copyable `return *this`, suggest `return move *this;` / copy() |
| Q2 | p3/alias-return-byvalue-param-leftovers-after-b21 | item 5 ratified (extern C callee owns by-value params) - no code |
| Q3 | p2/integer-pointee-pointer-conversion-accepted | char family: stage 1 retype core text APIs to char*, stage 2 block i8* <-> char* |
| Q4 | p2/lowered-cxx-field-structs-in-containers-and-array-fields | test_move recv_temp_snapshot leak count may go down |
| Q5 | p2/lowered-cxx-field-structs-in-containers-and-array-fields | keep destroy + rebuild; FIX user destructor running 3x on prvalue assign with a C++ field (scratch/q5) |
| Q6 | p3/cpp-per-group-static-same-name-binds-first | refuse as ambiguous, name both headers |
| Q7 | p3/cpp-overload-variadic-and-conversion-gaps | item 1: pairwise ranking like clang over variadic sets |
| Q8 | p4/immovable-attribute | `[immovable]` surface approved as proposed - buildable |

## Parked - needs a maintainer ruling before any work

| Issue | Question |
|-------|----------|
| p4/string-functional-construction-spelling | p4: spelling ruling needed |
| p3/cpp-constructor-call-accepts-implicit-narrowing | scalar conversion table at C++ call arguments (ctor AND free function): master's free path refuses widening like int -> double / short -> double / bool -> double for C++ callees while the 2026-09-04 ruling refuses only narrowing; ctor and free differ in 80/160 cells (scratch/c1_matrix_r3.md). Rule: accept widening everywhere, or keep exact-group only? |
| p3/cpp-unscoped-enum-to-double-param-refused | joins the scalar conversion table ruling above: CFlat refuses int -> double at every call argument (native too), so enum -> double would be a new rule; also `P4(0)` (C++ ambiguous int -> long vs pointer) (D1, 2026-09-28) |
| P0 remaining cold cost | eager define passes: keep eager or rule a demand-only bindable surface |

## Conflict map (never concurrent)

B3, C1, B4, D1 all touch LLVMBackend_Overloads ctor/argument selection - run them one after another
(order C1 -> B4 -> B3 -> D1, or D1 first as the cheap batch). D2 and D3 share operator dispatch. A1 and
A3 share the incremental action. G1 -> G2 -> G3 share the request-cache key. E1 touches
RequestCxxBraceConstructor, which B2's template spelling calls into - not concurrent with B2.

## Timebox 2026-09-30 - p2 burn-down (CLOSED 21:30, all runs landed; handoff scratch/resume_2026-09-30.md)

Landed: W1-W7, W9-W13 runs plus the alias-sink callee message (14 landings). Nothing in flight, no
worktrees left. Not started: W8 (DEFERRED, ruling: relocatability predicate for
p2/cpp-struct-list-field-relocated-bitwise phase 0).

Rulings given 2026-09-30 (recorded in the issue files): `L` = target C long; lambda by-value
captures owned by the closure (writes persist); core exports no external functions, users may
override libc names; `return *o` via a borrow is refused; alias-return functions stay refused as
function values.

Windows gate: scratch/win_gate.ps1, ship `git bundle create x <branch> ^26b8647e`; baseline
test.bat all pass, test_example 94/4/42 (4 = SSH-env GUI failures). Treat Windows timings as
pass/fail only (the maintainer games on that machine; cold header cache after a version flip).

## Timebox 2026-10-01 - rulings burn-down (22:33 2026-09-30 to 10:33 - extended 4 h at 03:15, land-only from 09:00)

Rulings from 2026-09-30 evening (memory rulings-2026-09-30-p2-batch). Max 3 implementers, Codex
Luna; escalation Codex r1-3 -> Opus -> Fable. Windows box shared with the maintainer: timings
pass/fail only.

| Run | Issue | Worktree / branch | State |
|---|---|---|---|

Landed: V20 52a77168 (field initializer in a no-arg user ctor built in place, single-call initializers only so a ternary takes the selected arm (also fixes the synthesized-ctor ternary); nested no-default-ctor refusal for `W w;`; Codex r1 + main-session r2 after the review caught a ternary wrong-value regression; leftovers p2/cpp-class-field-default-construction-leftovers-after-v20), V15 52a77168 (common expression statements at C++ header namespace scope give a clean clang: error instead of a Sema crash; per-statement extension switch-off, never in system headers, RAII; Codex r1-r3 + Opus r4 (r3's 18 "regressions" were a stale same-version cheaders cache); 9 remaining forms stay in the p2 issue), V18 52a77168 (C++ class field with no initializer default-constructed in place on every construction path incl. user ctors; no-callable-default-ctor refused lazily at the use site; 2 Codex rounds; follow-ups p2/cpp-class-field-default-construction-leftovers-after-v20), V19 91aeff46 (crash handler: SA_NODEFER + _exit on re-entry, so a fault in the state dump no longer spins at 100% CPU; dump kept, registry lock via try_lock; Codex r1 root cause (deadlock) was wrong, main-session sample found the refault loop), V17 19018a0c (classifier item 12: generic own methods, `->` paths, alias index/++ stores, field-path aliases, this to helpers, element-address escapes, direct-param index stores; follow-ups item 13), V14 52a77168 (C++ internal-linkage namespace variables, constants included, one copy per import group: __cflat_sv_<group>_ names, demand companions map back, COFF comdat moves with the rename; cache 163; 3 Codex rounds + main-session conflict-identity and comdat fixes; follow-up p3/cpp-per-group-static-same-name-binds-first), V16 19018a0c (copy-on-entry classifier items 10-11: aliases, this-alias, getter chains, escapes, ++/--, hashset, parse-tree call detection; 2 rounds; leftovers item 12 -> V17), V1b 52a77168 (W8 phase 3: lowered C++-field structs copied/moved/assigned like C++, construct into released, list slots in place; cache 161; 5 rounds, r4 Opus, r5 Fable; leftovers p2/lowered-cxx-field-structs-in-containers-and-array-fields; ruling note: test_move recv_temp_snapshot leg encodes a removable leak), V13 19018a0c (join-arm new into a late-defined or function-value CFlat callee survives unwind; residual in p3/join-arm-small-gaps-after-w4), V12 19018a0c (`return *o` / `*h->q` / `a[0]` of an owning struct through a borrow refused, suggest `return move`; owned `move T*` param implicit move; ruling question on non-copyable `return *this` in the p2 issue), V5b 52a77168 (off MSVC an address-only plain C inline is not requested: fixes cold-cache double parse of c_macro_helpers.h), V5 52a77168 (C/C++ header inline bodies emitted on demand like clang: static-inline thunks with comdat, private call copies, &fn on the real symbol, MSVC comdat body; cache 160, c-inline-v9; 5 rounds, r5 Fable; follow-ups p2/cpp-internal-static-var-merged-across-imports, p3/c-inline-bodies-no-debug-info), V11 52a77168 (user C-linkage definition of a libc name is nobuiltin: wins at -O2; follow-up p3/core-libc-prototype-types-mismatch), V10 19018a0c (owning return of a not-yet-complete type: bare discard refused at the call, explicit discard destroyed; limit: expect_error scope sees the error late when the struct is defined after the block), V9 19018a0c (generic T* from a fixed array; brace lists of pointers refuse integer-pointee mismatch), V8 19018a0c (L = target long, LL = i64), V4b 19018a0c (integer-pointee gaps: array decay, view ternary arm, brace field, global init), V1 19018a0c (W8 phases 1-2: sret + in-place construction for structs with non-relocatable C++ fields; phase 3 = V1b), V7 19018a0c (lambda by-value captures persist), V2 19018a0c (alias-return leftovers; extern C callee owns by-value params - ratification item 5 in the p3 issue), V6 closed without code (profile: copy-on-entry walks 11 ms = 1.3% of test_collection_leaks CodeGen, within the 2% target; log scratch/repro_keep/v6), V4 19018a0c (stage 1; issue stays open for gaps + char family), V3 52a77168 (define-group ODR error; follow-up p3/cpp-define-odr-error-signature-positions).
