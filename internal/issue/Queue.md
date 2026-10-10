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

**Maintainer 2026-10-06: both the Mac and the Windows box are in interactive use (Windows = gaming) - no perf / wall-clock measurements on either; gates report pass/fail counts only; libs_perf.sh skipped.** **Maintainer 2026-10-06: tier 3 (torch) is SKIPPED until further notice** - stdlib is the focus; the gate is tiers 1-2
(plus test.sh / examples). libs_perf.sh torch legs are skipped with it.

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
| T70 | fix/t70-stdmove | p2/std-move-xvalue-argument-positions | Luna + Opus round 2 |
| T74 | fix/t74-globarr | p2/global-array-of-cflat-struct-not-default-constructed | Luna |
| T75 | fix/t75-strcast | p3/string-cast-of-string-literal-refused | Luna |


## A - crashes, asserts, link failures

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| A3 | p3/cpp-incremental-retry-after-failed-parse-crashes | ModuleBuilder / IncrementalAction GenModule | latent (t14-t16, t31 cold retries); known trigger closed. Same subsystem as A1 - run AFTER A1 lands |

## B - wrong value / wrong overload pick

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| - | p3/ternary-of-int-literals-typed-long | ternary result typing | `k ? 5 : 6` typed long -> template deduces T=long (T54 review 2, pre-existing) |
| - | p3/cpp-ctor-template-overload-and-copyinit-gaps | C++ ctor-template ranking | unnamed-enum source picks int ctor; non-const trivially-copyable lvalue copy-init skips U&& template (T54 review, pre-existing) |
| - | p3/cpp-ternary-arith-type-at-ctor-and-assign-operator | ternary typing at C++ ctor arg / operator=(T) | rv.C(k ? 5 : 6) and operator=(T) deduce signed char, clang int (T55 review, pre-existing; AFTER T55) |
| - | p3/same-named-local-enums-in-two-functions-collide | local enum registry keying (non-generic) | two functions with `enum E` refused as already defined; local shadowing file-scope enum refused (T60 review, pre-existing; AFTER T60) |
| - | p3/cpp-deleted-function-call-passes-check | C++ overload resolution | rv.a.del(1) on `= delete` passes --check (T59 review 2, pre-existing) |
| - | p3/cpp-explicit-static-ctor-call-initializer-refused | explicit-type static from a C++ ctor call | `static T g = T(a);` / ternary of ctor calls refused (shallow-copied field); T62 review 1, pre-existing |
| - | p3/auto-copy-of-interface-or-closure-variable-loses-callable | auto copy of iface / closure var | `auto r = ifaceVar; r.get()` and `auto f = lam; f(1)` fail; T62 review 1, pre-existing |
| - | p3/static-local-init-guard-set-before-throwing-initializer | static init exception safety | guard set before initializer; throwing C++ init leaves a zeroed object, never retried; T62 review 2, pre-existing |
| - | p3/cpp-anonymous-typedef-struct-in-namespace-not-found | anon typedef struct in C++ namespace | `w2.TS` not found (global scope works); T61 review 2, pre-existing |
| - | p3/cpp-wrapper-rejection-cache-keyed-by-text-only | stale cached wrapper rejection | TU-context-dependent clang rejection replays after a fix until cache cleared; T58 r3, pre-existing |
| - | p3/cpp-brace-ctor-enum-element-overload-choice | brace ctor with enum elements | A({e,e}) picks unsigned[2], L({e,e}) ambiguous; clang promotes to int; T58 review 3, pre-existing |
| - | p3/cpp-initializer-list-only-class-empty-braces-refused | `T v{}` with only an initializer_list ctor | refused; clang calls it with an empty list; T63 review 1, pre-existing |
| - | p2/cpp-by-value-class-temporary-extra-moves | prvalue C++ class into C++ (by-value args, brace elements, by-value param return) | extra move/copy + dtor vs clang elision; deleted-move prvalue refused; N67 partial; raised p3 -> p2 2026-10-09 |
| - | p3/cpp-brace-element-enum-and-macro-constants-not-constant | enum/macro constants in braces | refused as narrowing (non-constant); clang accepts; T63 review 2, pre-existing |
| - | p3/hex-literal-above-int-max-typed-int | hex literal typed int, sign-extends | RULED 2026-10-09: C/C++ ladder (0xFFFFFFFF = unsigned int); `u64 & 0xFFFFFFFF` does not mask today; decimal/octal/binary already right |
| - | p3/cpp-class-global-brace-initializer-refused | global `std.vector<int> gv{1,2,3};` | refused with misleading message; T63 review 2, pre-existing |
| - | p3/cpp-variadic-function-inline-body-link-failure | inline C++ variadic fn | `vsum(int, ...)` with inline body fails to link; T58 review 4, pre-existing |
| - | p3/auto-local-of-arithmetic-takes-cflat-width-type-at-cpp-calls | auto local at C++ template call | `auto x = 1e3 + 1; fwd(x)` float / refused, `auto x = l + 1` long long; T58 review 5, pre-existing |
| - | p2/cpp-header-namespace-expression-statement-segfault | C++ namespace statement predictor | RESIDUE after T66: `x < y;`, `x > y;`, `x << 1;`, `decltype(x)(1);`, `int(x) + 1;` still crash; needs Sema / ParseTopLevelStmtDecl-side fix |
| - | p3/cpp-global-of-template-specialization-field-access-refused | C++ global of template type | `r67.p11.u` (P<int,int> global) "not a member of namespace"; T66 review 2, pre-existing |
| - | p3/same-variable-postfix-twice-in-one-call | `two(y++, y++)` | both args 0 and one increment lost; T67 review 1, pre-existing |
| - | p3/simd-float-postfix-increment-generic-message | simd<float,4>++ | generic lane-count error; decide lane-wise or specific refusal; T71 review 1 |
| - | p3/cpp-field-given-in-brace-init-also-default-constructed | `{ m = mm }` on a C++ field | extra default ctor/dtor pair before the copy; T73 review 1 |
| - | p3/cpp-wrapper-request-misses-template-argument-headers | `std.unique_ptr<cppon.Cls>` wrappers | importer-only header group; hidden by incremental; T70 round 2 |
| - | p3/cpp-ternary-of-pointer-lvalues-at-reference-param-binds-copy | `t ? p : o` at C++ `int*&` / `const&` | binds a temp (clang binds the arm); T67 review 2, pre-existing |
| - | p3/cpp-nullptr-into-const-nullptr-t-ref-passes-null-reference | null temp at C++ reference params | nref(nullptr) passes ptr null as the reference (T52 r5, pre-existing; AFTER T52) |
| B5 | p3/cpp-const-twin-overloads-collapse | C++ class member registration (const / non-const twins share one CFlat signature) | NEW 2026-09-28 (B1 matrix, OUT cells C1/C4, E1-E5, F1/F4, H1/H4/H9, L10) |
| B5b | p3/cpp-constrained-twin-default-argument-ranking | C++ class member registration (constrained same-signature twins with a default argument share one CFlat signature) | NEW 2026-10-03 (T22 review rounds 2-4; pre-existing on master) |
| B5e | p2/cpp-reference-member-reads-as-pointer | C++ record field of reference type (pair<const int&,...>.first) | NEW 2026-10-03 (T31 r3, pre-existing on master): reads the address - wrong code in printf |
| B5f | p3/cpp-conversion-operator-copy-init-gaps | C++ copy-init through operator T(): trivial target + ctor/operator ambiguity | NEW 2026-10-03 (T35 Opus review, pre-existing on master) |
| B5g | p3/cpp-callback-pointee-const-ranking-gaps | function-pointer values lose pointee const; clang-refused callback sets accepted | NEW 2026-10-03 (T25 rounds 3-5, pre-existing on master) |
| B5h | p3/cpp-nested-member-class-free-return-and-call-result-assign | free function returning a nested member class; assignment to a class result of operator()/operator* | NEW 2026-10-03 (T30 review 2, pre-existing on master) |
| B5i | p2/cpp-forwarded-fnptr-variable-or-closure-dangles-in-wrapper-frame | function-pointer variable or closure forwarded to a C++ `T&&` whose reference escapes through the result (wrapper-frame slot) | NEW 2026-10-03 (T28, sibling of the fixed literal case) (2026-10-06: fn-pointer half landed T56 393f0bd9; escaping closure refused - closure binding remains) |
| B5j | p3/cpp-pointer-ref-overload-ctor-and-direct-member-ranking | `const char *const &` overloads: constructor wrapper path and direct non-template ranking; literal at a `const char *&&` template refused | NEW 2026-10-05 (T28 round 5 matrix, pre-existing on master) |
| B12 | p2/cflat-extern-definition-abi-leftovers | function<> from C with a >16B struct param segfaults | RULED 2026-10-09: every function<> call uses the C ABI; CFlat fns get a static wrapper when bound (raised to p2) |
| B14 | p3/c-flexible-array-member-leftovers | move a->data (RULING: explicit move of a raw pointer) + 3 pre-existing P3 | OPEN (sizeof/alignof landed 118c2769) |
| B16 | p3/cpp-operator-move-operand-leftovers | template U&&, elision, move into const&-only ruling | NEW 2026-09-28 (D8 report) |
| B18 | p3/cpp-assignment-result-leftovers-after-b9 | extra copies (paren call arms, arr elem init) + bare scalar/nested ternary refusals + 3 unrelated | OPEN (P2 landed 1955bd95) |
| W13b | p3/cpp-overload-variadic-and-conversion-gaps | variadic-set ambiguity, defaulted param before ellipsis, member variadics, user conversions at C++ call args, inline variadic link, non-record const pointers | NEW 2026-09-30 (W13 reviews) |
| G6 | p3/header-cache-residual-growth | in-version prune age rule (sigbase, dead request configs, cxxdemand) | NEW 2026-09-29 (split from G5) |
| B2 | PARKED 2026-09-28 after 3 Codex rounds (scratch/b2_parked.patch): p3/cpp-string-literal-template-deduction-and-unspellable-char-args + p3/cpp-std-min-long-and-pointer-arguments-refused. Free-array issue closed (premise disproved, [over.ics.rank]/3.2.1). Restart from master on opus with the narrower design in the issue files | RequestCxxFunctionTemplate argument spelling (CxxStringLiteralSpelling, InferImplicitCxxArgumentType) | NEW. One mechanism: spell array / literal lvalues as `*reinterpret_cast<E (*)[N]>(p)` like the member path (e6c60220); give `long` arithmetic, `char*` and pointer-arithmetic rvalues a C++ spelling |
| B3 | p3/cpp-pointer-argument-prefers-char-pointer-over-converting-ctor + p3/cpp-single-level-pointer-unproven-at-call-argument + p3/cpp-operator-address-of-operand-not-converted | IsProvenPrimitiveSinglePointerArg / CompareUpconvert | old bucket 2b (2a landed as AT). Pointer-is-not-a-number ruling |

## C - wrongly accepted (adds a rejection)

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| - | p3/cpp-virtual-dtor-invalid-operator-delete-accepted | CxxIncrementalGroup.cpp LazyBodies | deleted/private/ambiguous op delete of a skipped virtual dtor body never diagnosed (T50-win review, pre-existing) |
| - | p3/macro-constant-treated-as-lvalue | macro provenance / lvalue checks | &M, M = 5, int& binds macro global, ov picks int& (T52 review, pre-existing; run AFTER T52) |

## D - wrongly refused (acceptance gaps)

| Run | Issue(s) | Site | Mode |
|-----|----------|------|------|
| - | p3/cpp-null-macro-and-nullptr-spellings-refused | null-constant classifier + converting ctor | full; NULL/__null/nullptr macros, nullptr -> std::function (T52 review, pre-existing; AFTER T52) |
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
| - | p3/cpp-relayed-clang-text-wrong-scope-for-missing-member | relayed clang text | rv.nosuch(1) -> "no member named 'rv' in the global namespace" (T53 review, pre-existing) |
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
| N3 | p2/cpp-const-object-nonconst-member-call-writes-readonly | loop back-edge const-pointer dataflow, virtual-base twin | W13 landed const auto copy, const globals, const-pointer overload selection 2026-09-30; rest open |
| N7 | p2/o2-builtin-folding-overrides-cflat-definitions | test_cpp_interop.cb still fails at -O2 (non-unwind leftovers) | NEW 2026-09-29 |
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
| N22 | p3/cpp-template-ctor-deduction-enum-and-arith-rvalue | C++ template constructor deduction - enum lvalue and arithmetic rvalue deduce the wrong T | NEW 2026-09-29 |
| N23 | p3/cpp-unary-operator-leftovers-after-d7 | Unary operator leftovers after D7+C3 | NEW 2026-09-29 |
| N24 | p3/cpp-unrequested-specialization-pointer-eager-request | Bucket: p3 (C++ interop compile time; left by D6, fix/unrequested-spec-pointer-retry, 2026-09-29) | NEW 2026-09-29 |
| N25 | p3/pointer-to-number-diagnostic-hint-leftovers | Pointer -> number store refusal: diagnostic hint leftovers | NEW 2026-09-29 |
| N26 | p3/return-keep-rule-later-defined-callee-leaks | Bucket: p3 (ownership temporaries; left by E2, fix/coalesce-arm-temp-gaps, 2026-09-29) | NEW 2026-09-29 |
| N27 | p3/cpp-default-wrapper-tie-list-shows-wrapper-shape | Ambiguous call through a default-argument wrapper lists wrapper shapes, not declarations (R3 review) | NEW 2026-09-29 perf |
| N28 | p3/cpp-deferred-special-members-mid-size-harvest-cost | R1 deferral costs +1-2% cold on 64..~500-record harvests (interop fixtures); cutoff adjustable later (maintainer) | NEW 2026-09-29 perf |
| N31 | p3/cpp-interpreter-drops-cc1-only-flags | -Xclang cc1-only flags silently dropped by the Interpreter driver (pre-existing, H1 review) | NEW 2026-09-29 perf |
| N32b | p3/cpp-ctor-refusal-text-gaps-after-w11 | default-member-init refusal lacks clang: prefix, ctor template names wrong overload (W11 landed the body-failure fix 2026-09-30) | NEW 2026-09-30 |
| N33 | p3/cpp-invalid-virtual-body-link-failure | Invalid virtual body of a C++ class template = link failure, not a use-site error (pre-existing, R4 review) | NEW 2026-09-29 perf |
| N34 | p3/cpp-deployment-target-env-core-cache-miss | Non-default MACOSX_DEPLOYMENT_TARGET misses the core bitcode cache every compile (R4 review) | NEW 2026-09-29 perf |
| N35 | p3/cpp-demand-bodies-torch-cold-cost | R4 costs +4.4% cold instructions on torch (json -28%); verdict writes batched in 93b361f6 (-0.55%), rest is clang instantiation | PART 2026-09-29d |
| N36 | p3/cpp-demand-second-use-of-failed-helper-generic-text | Second use through an already-failed helper refused without clang's text (R4 review) | NEW 2026-09-29 perf |
| N37 | p3/cpp-signature-registration-projects-records | Registration projects every record a signature names; -3.6% torch only by skipping projection, which breaks overload order (D4) | NEW 2026-09-30 perf |
| N41 | p3/cpp-variadic-alias-template-refused | in-repo `template<auto... I> using A = S<I...>` refused (alias machinery has no pack support) (ST4, pre-existing) | NEW 2026-10-01 |
| N42 | p3/cpp-header-unreferenced-global-initializers-skipped | unreferenced C++ header inline globals never run their dynamic initializer (referenced ones + .cpp sources fixed/fine, T44) | DEFERRED p3 2026-10-09 (maintainer: not a blocker, later) |
| N43 | p3/unsigned-enum-cast-case-label-sign-extended | native `enum E : u8`, `case (E)200:` becomes -56 (ST4 review, pre-existing) | NEW 2026-10-01 |
| N47 | p3/cpp-constant-fold-and-const-static-twin-leftovers | constexpr-call initializer refused by the fold guard; const receiver + static twin overload refused (ST4 re-review leftovers) | NEW 2026-10-01 |
| N52 | p3/cpp-range-for-member-name-lookup-and-count-probe | range-for member lookup misses enum/nested-type begin/end (accepts invalid); count/get clients pay a failed ADL probe; ST6 follow-up | NEW 2026-10-01 |
| N54 | p3/cpp-nullptr-t-returns-and-deduction | nullptr_t returns, const& params, deduction, defaults refused (master too); ST3 follow-up | NEW 2026-10-01 |
| N57 | p3/lowered-struct-byvalue-pass-no-moved-diagnostic | lowered struct passed by value: later read not diagnosed as moved; Q5 follow-up | NEW 2026-10-01 |
| N58 | p3/cpp-struct-identity-underscore-namespace | [cpp] identity: ns_ namespace decode, global name collision, CRTP base reading a derived field (master too); N51 follow-up | NEW 2026-10-01 |
| N59 | p3/cpp-struct-self-return-by-value | [cpp] struct self() by value: POD source zeroed, copyable [cpp] field refused as deleted copy ctor (master too); Q1 review follow-up | NEW 2026-10-01 |

## std smoke suite gaps 2026-10-01 (test_libs/std DISABLED cases; group at the next queue build)

`DISABLED_WIN` / `DISABLED_MAC` disable a case on one platform only (MSVC-only / libc++-only gaps). Fixing a row removes the DISABLED marker of its case(s) in the same commit (`./test_libs.sh std --include-disabled` shows XPASS).

| Run | Issue | DISABLED case | Summary |
|---|---|---|---|

## std_full tier-2 gaps 2026-10-02 (test_libs/std_full DISABLED cases; plan internal/plan/std-library-tier2-suite.md)

`DISABLED_WIN` / `DISABLED_MAC` disable a case on one platform only (MSVC-only / libc++-only gaps). Fixing a row removes the DISABLED marker of its case(s) in the same commit (`./test_libs.sh -t 2 std_full --include-disabled` shows XPASS). Wrong-code / crash rows first: valarray literal operand, valarray ptr ctor, optional<string> deref compare, errc conversion, insert_iterator copy, wcs* recursion, char[N] verifier failure, noop_coroutine LLVM fatal.

| Issue | DISABLED case | Summary |
|---|---|---|
| p3/cpp-reference-param-conversion-gaps-after-t42 | (C++ interop) | template ctor from non-template source, inherited operator X(), operator X&/X&& into C++ ref params refused (clang accepts; T42 follow-up) |
| p2/pointer-to-unrelated-primitive-pointee-accepted | (none) | single g(double*) accepts int* silently (clang refuses); confirm integer-pointee ruling extends to floating pointees |
| p3/cpp-template-deduction-identifier-with-e-read-as-double | (C++ interop) | `1 + abcer + 1` deduces double (text-classified literal); fix by source type, never text |
| p3/static-auto-prvalue-and-copy-initializer-storage | (none) | `static auto g = make();` / `= s;` stack storage + per-call destruction (T45 fixed only the xvalue arm) |
| p3/enum-class-assignment-from-wrapped-enum-cast | (C++ interop) | `o = (E(x))` / `o = g(E(x))` / macro-wrapped refused into a class with an enum ctor; classify by value identity, not text; T53 batch attempt dropped 2026-10-06 (not one-site, more wrappers) - FULL mode |
| p3/if-const-grouped-logical-ternary-not-folded | (none) | `if const ((a && b))` / grouped `?:` refused (scratch loads/PHIs not folded); clang accepts |
| p3/cpp-pointer-depth-and-array-pointer-param-gaps | (C++ interop) | non-null int** accepted into C++ int***; member/static array-pointer params refuse valid args |
| p3/cpp-nullptr-t-variable-free-operator-operand | (C++ interop) | `nn == n` with a std.nullptr_t variable refused for a free operator (literal nullptr works); T48 follow-up |
| p3/float-literal-plus-variable-computed-in-float | (C++ interop) | `i + 1.0` / `f + 1.0` typed float (clang double); check CFlat literal rule first |
| p3/generic-overload-not-joining-concrete-overload-set | (none) | generic f<T>(T*) never joins concrete overloads; 12 clang-valid calls refused |
| p3/member-type-path-remaining-gaps | (none) | `Outer<int>.Inner<long>.type`, closure-alias members, CFlat static data members, C++ statics as value template args (T37 follow-up) |
| p3/generic-later-record-in-signature-or-local | (none) | generic body: later record in a signature / by-value local refused (master too; T37 follow-up) |

## Ruled 2026-10-01 - ready to schedule (rulings recorded in each issue file)

| Ruling | Issue | Work |
|---|---|---|
| Q2 | p3/alias-return-byvalue-param-leftovers-after-b21 | item 5 ratified (extern C callee owns by-value params) - no code |
| Q3 | p2/integer-pointee-pointer-conversion-accepted | char family: stage 1 retype core text APIs to char*, stage 2 block i8* <-> char* |
| Q4 | p2/lowered-cxx-field-structs-in-containers-and-array-fields | test_move recv_temp_snapshot leak count may go down |
| Q8 | p4/immovable-attribute | `[immovable]` surface approved as proposed - buildable |
| Q9 | p4/three-way-comparison-operator | `<=>` in the CFlat language RULED 2026-10-02, plus defaulted member-wise `==` - buildable (result type without <compare> follows C++ unless told otherwise) |

## Ruled 2026-10-09 - ready to schedule (rulings recorded in each issue file)

| Issue | Ruling |
|---|---|
| p3/hex-literal-above-int-max-typed-int | C/C++ literal ladder (0xFFFFFFFF = unsigned int); C23 `wb` slots in later |
| p3/cpp-unscoped-enum-into-other-enum-param-accepted | enum -> other enum needs an explicit cast (enums are type-safe); rewrite the pinning leg |
| p3/cpp-constructor-call-accepts-implicit-narrowing + p3/cpp-unscoped-enum-to-double-param-refused | C++ call args (ctor and free): widening accepted, narrowing refused |
| p2/pointer-to-unrelated-primitive-pointee-accepted | refuse unrelated primitive pointees (int* -> double*) on the single-candidate path |
| p2/o2-builtin-folding-overrides-cflat-definitions (item 2) | -O2 may omit global new[]/delete[]; rewrite counter legs, no IR change |
| p3/cpp-operator-move-operand-leftovers (B16 ruling item) | block `move d` into a const T&-only operator (const T& is a borrow), same as f(move d) |
| p2/cpp-reference-member-reads-as-pointer | C++ `T&` member = `alias T` field; rewrite the 3e5f2260 stopgap legs (T47) |
| p3/iface-return-pointer-arithmetic-refused | accept `return t + 1;` into an interface; edit the backstop leg (T36 follow-up) |
| p2/cflat-extern-definition-abi-leftovers (B12) | function<> calls always use the C ABI; static wrapper for CFlat fns |

## Parked - needs a maintainer ruling before any work

| Issue | Question |
|-------|----------|
| p3/cflat-unique-dot-dispatches-to-pointee-member | `.` on CFlat unique<T>: prefer unique's own members (like C++ smart pointers after T46) or keep forwarding? |
| p4/string-functional-construction-spelling | p4: spelling ruling needed |

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

## Timebox 2026-10-01b - std smoke suite burn-down (goal: land all S1-S20)

Runs group the S rows by mechanism. Each run removes the DISABLED markers of its cases in the fix commit (`./test_libs.sh std --include-disabled` -> XPASS). Max 3 implementers, Codex Luna; escalation Codex r1-3 -> Opus -> Fable. Windows box off: no Windows gate this timebox (20:45: box back; master validated - test.bat green, std tier 7 Windows-only failures -> N55). 21:05 maintainer: the max-3 implementer cap is the Mac's; up to 3 more runs build/test on the Windows box (edit on Mac, winsync, Mac gate at landing). 20:08 maintainer: Claude slots 3 (= codex). 20:05 maintainer: timebox reset to 4 hours from now - ends 00:05, land-only from ~23:05.

15:55 maintainer experiment: implementers = Opus (Agent tool), reviewers = Codex Sol 6.1 (gpt-6.1-sol, high); up to 3 codex + 2 claude agents, max 3 active implementers; the Luna runs already started finish as Luna. Machine shared with the maintainer's other work: compile-time numbers are unstable - libs_perf.sh is run but only a gross regression (> 25%, re-measured once) blocks a merge; note the numbers per landing.

| Run | Rows | Mechanism | Wave |
|---|---|---|---|
| T2 | S3, S6, S11 | C++ free function whose return type is an implementation / reference-tuple type (`__pipeable`, `__not_fn_t`, `tuple<T&...>`): wrapper cannot be registered | 1 |
| T3 | S7, S9, S14, S15 | template-argument spelling: explicit args on a function template call (`duration_cast<seconds>`), defaulted args (`barrier<>` / bare `barrier`), `T[]`, function-type deleter | 1 |
| T4 | S4, S10, S16, S18, S20 (+ rest of N21) | static / namespace entity lookup and values: `is_null_pointer<nullptr_t>::value` wrong value, `std.byte`, `integer_sequence::size()`, `std.numbers.pi`, `std.endian.native` | 1 |
| T1 | S1, S2 | CFlat callable -> C++: lambda / function<> / fn name into `std::function` and into a deduced callable param; after T2 (same wrapper code) | 2 |
| T5 | S12, S19 (S17 moved to ST2) | call-argument / operator lookup: char buffer to `to_chars`, `path operator/` (hidden friend), `format_to_n`. S12 touches B2's char-array spelling (parked) | 2 |
| T8 | S5 | std::move kept (ruling): `T&&` C++ call result as init / assign / return move source + diagnostics; ownership area, opus-tier review | 2 |
| T6 | S13 (+ std_20_94 range-for) | range-for over a C++ range, member or ADL begin/end (`directory_iterator`, views) picks a CFlat `count()` path | 2 |
| T7 | S8 | CRTP C++ template base on a `[cpp] struct` (fails even with `[cpp]`: "not a C++ class"). Case moves to `[cpp] struct` per R2 (plain struct never a C++ template arg) | 3 |

Conflicts: T1 after T2 (wrapper registration); T3 and T5 both spell C++ template / call arguments - not concurrent. Filler when a slot frees (ruled, ready): Q6, Q1, Q5, then Q7 (W13b). Q3 (staged char family) and Q8 ([immovable] feature) only after every T run landed.

| Run | Issue | Worktree / branch | State |
|---|---|---|---|
| N54 | nullptr_t returns/deduction (Windows-box run) | cflat-fix-n54 + Win C:\source\cflat-wn54 | Opus r1 21:07-21:58 (4 gaps, defaulted already worked), Win gate green except N60/N55 known; rebased; Opus reviewer r1 22:00-22:06 FIX FIRST (P1 non-null pointer expr deduced nullptr_t; P1 int* instantiation reused for nullptr - leg 2871 fails on macOS; P2 phantom void* ambiguity); Opus r2 (Windows) 22:07-22:45 COMMITTED in worktree, rebased on 90a8a8d9 (P1s fixed; p12 void* after int* = master leftover); Opus review r2 interrupted at wind-down - NEXT: review + Mac gate |
| N46 | header edit after warm build -> inline body fails (p2) | cflat-fix-n46 / fix/cpp-header-edit-warm-cache | Opus r1 21:28-22:45 COMMITTED in worktree, rebased on 90a8a8d9: not reproducible at HEAD (hidden by 90a8a8d9), real defect fixed - a group re-included an unguarded header in a request prelude (CxxIncrementalGroup.cpp); repro scratch/n46_repro2.sh; Opus review interrupted at wind-down - NEXT: review + gate |

Landed: ST2 90a8a8d9 17:57 (S6 tie, S11 not_fn, S17 path /, S3 trimmed: views compose, range-for -> ST6; ADL + hidden-friend operators with a conservative skip guard; compound ops call the written operator; cache 164; leftovers N44). ST4 90a8a8d9 18:08 (S4 is_null_pointer, S10 std.byte, S16 index_sequence size, S18 numbers.pi, S20 endian + all of N21 static data member forms; fold only constant-initialized side-effect-free variables; cache 165; leftovers N45, N47). ST8 90a8a8d9 20:07 (S5 std::move; gate 1216/0/8, ex 45/0, t1 37/0, t2 45/0, torch 11/0, cold 151 s / warm 4 s). ST6 90a8a8d9 20:19 (S13 directory_iterator + std_20_94 composed views; member else ADL-only begin/end, private nested iterators via friend-injection alias, lvalue range; gate 1216/0/8, ex 45/0, t1 39/0, t2 47/0, torch 11/0, cold 164 s / warm 6 s; leftovers N52). ST7 90a8a8d9 20:38 (S8 [cpp] struct CRTP base, enable_shared_from_this; gate 1218/0/8, ex 45/0, t1 40/0, t2 48/0, torch 11/0, cold 186 s / warm 5 s - creeping, re-measure; leftovers N51). ST1 90a8a8d9 20:36 (S1 lambda -> std::function, S2 callables -> deduced C++ params: non-capturing as fn pointers, captures refused -> N50 ruling; alias-param guard main session; gate 1218/0/8, ex 45/0, t1 42/0, t2 50/0, torch 11/0, cold 152 s / warm 5 s; leftovers N53). ST3 90a8a8d9 + ST5 90a8a8d9 21:03 (one gate on the stack: S9 duration_cast, S14 barrier bare, S15 make_unique/make_shared arrays + unique_ptr<T[]>::reset<U>, nullptr_t mark, cache 166; S12 to_chars/from_chars, S19 format_to_n: char* values not spelled const char*, const pointee kept for C++ results and fields, cache 167; gate 1222/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 163 s / warm 6 s; leftovers N54; std DISABLED now 1 (S7 needs a ruling)). N48 90a8a8d9 21:02 (CFlat `move` of a C++ class mimics std::move; deleted move special refused; gate 1224/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 188 s at load 21; leftovers N56). N51 90a8a8d9 21:30 ([cpp] struct identity: __cflat_user.<name> template args canonicalized, explicit std.weak_ptr<Self> locals bind; gate 1222/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 147 s; retest on N48 1224/0/8; leftovers N58). Q1 90a8a8d9 21:45 (non-copyable `return *this` refused like `return *o`; gate 1224/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 190 s at load ~20; leftovers N59). N60 + Q7 on one gate 21:50: N60 (Windows-only NuaAnon regression from 90a8a8d9 - anonymous struct members carry clang's triviality flags, cache 170; Win gate green) and Q7 (variadic sets ranked pairwise like clang, clang-ambiguous sets refused; leftovers appended to p3/cpp-overload-variadic-and-conversion-gaps); gate 1226/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 173 s / warm 7 s. Q5 90a8a8d9 22:15 (user destructor once per prvalue assign with a copy-only C++ field: moved-from shells torn down C++-fields-only at the normal cleanup slot, paren/explicit-move returns too; Luna r2 + main-session sticky lock probe; leftovers in the p2 issue; gate 1226/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 180 s / warm 7 s). N55 + N56 + Q6 on one gate 22:12: N55 90a8a8d9 (std tier on MSVC STL: all 7 Windows std failures fixed - partial-instantiation member templates, operator() templates, static base operator(), ctor-wrapper import groups, unevaluated operands skipped, auto member returns; cache 171), N56 90a8a8d9 (move edge shapes mimic std::move; cache 172; leftovers p3/cflat-move-cpp-class-sink-shapes-leftovers), Q6 90a8a8d9 (per-group static same name ambiguous unless same entity; address consts refused, float bits; cache 173; leftover p3/cpp-namespace-constexpr-array-undefined); gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 159 s / warm 6 s. N49 90a8a8d9 + N61 90a8a8d9 22:23 (N49 Windows-box run: trivially copyable C++ xvalues assign/return like clang, trivial `b = move a` with assignment template runs clang's pick; N61 Windows regression from 90a8a8d9: base names canonicalized, MSVC members inherited from _Ptr_base/_Tree found; gate on N53 tip 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 150 s / warm 6 s). N53 90a8a8d9..90a8a8d9 22:40 (generic function names and plain names bind std::function by std::function's rule via a converting thunk; master wrong code f(-5) fixed; discarded destructible return refused; 3 Opus-reviewed rounds; gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 155 s / warm 5 s). N44 90a8a8d9 22:50 (exact hidden-friend / ADL operator beats a built-in via conversion like clang; Windows-box run; gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 135 s / warm 4 s).

Open questions for the end-of-timebox report (maintainer 18:25: no time to review now): std.move result binding a T* parameter (p2/std-move-xvalue-argument-positions); explicit `std.barrier<>`; S7 deleter encoding (separate function<> vs fn-pointer encodings, or retry).

Started 15:31, ends 00:05 (reset 20:05 to 4 h from then; land-only from ~23:05). Slot order as runs free up: T8, T6, T1 (after
ST2 lands), T5 (after ST3 lands), T7, then filler Q6, Q1, Q5.

## Timebox 2026-10-01c - land the std lib on macOS + Windows, then parity perf (23:40 to 07:40)

Goal (maintainer): std tier green on macOS and Windows (S7 included), Windows test.bat green; then compile-time parity, every test_libs parity case cold <= 1.3x clang++ first, then <= 1.1x (macOS, default PGO Release build). Roles: mixed - Codex Luna (high) implements -> Opus reviews; Opus implements -> Codex Sol 6.1 (medium) reviews. Escalation: 3 rounds -> Opus finishes -> Fable only when stuck. One issue per run (two only if they share a function); gates batched. Windows box: game running, timings pass/fail only.
Rulings 2026-10-02 00:10 on the 2026-10-01b report: (1) std.move T* binding follows N48, refused like clang (issue file); (2) `std.barrier<>` allowed, bare name stays (new p3/cpp-template-empty-argument-list-refused, run N63); (5) Q6 as landed (equal-valued const merged and folded); mutable statics refused and the user puts the conflicting headers in one import group - refusal hint p3/cpp-per-group-static-refusal-hint (00:20); (6) N48 confirmed. (4) N50 go-ahead 00:30: wrapper class, no new syntax (issue file).
Rulings 03:45 (maintainer leaving, low on usage): (a) skip LLVM VerifyModule on clang-generated companion modules in Debug and Release (CFlat-generated IR still verified); (b) a run that fails its round-4 review: strip the specific failing case and land the rest if it stands alone (leftover filed as an issue), otherwise hold the branch unmerged - no Fable. 03:50: "keep going, just not with Fable" - continue normal rounds without Fable. 02:57 (clock time): Sol reviews on hold for 24 h (until 2026-10-03 03:00); Luna for implementing only, Opus-written code gets a fresh Opus reviewer (same family, clean context); when Luna usage runs out, implementers switch to Sonnet 5.5 (Agent tool, model sonnet) and Opus keeps reviewing; ~03:45 maintainer turned in: Mac + Windows boxes quiet (perf numbers from here are comparable); Sol N63 r4 had already finished (FIX FIRST, P2 edge cases only -> leftovers p3/cpp-template-empty-argument-list-leftovers, N63 landing per the strip-and-land ruling).
Ruling 01:20: S7 template-argument spelling follows what the user wrote (internal only, no new syntax): `function<R(A)>` = C++ pointer R(*)(A); a bare `R(A)` in template brackets = function type R(A) (std.function, packaged_task, user Sig templates); replaces the std.function name exemption. `int*(int)` as a pointer spelling rejected (C meaning is function returning int*).
Ruling 23:55: S7 - a CFlat function<R(A...)> as a C++ template argument is spelled R(*)(A...); plain function names bind as that pointer (recorded in the S7 issue file).

| Run | Issue | Worktree / branch | Implementer -> reviewer | State |
|---|---|---|---|---|

Windows master 8d663b79 (N62+N54+PF1+N46) 02:00: test.bat all pass, examples 94/4/42 (4 SSH GUI), t1 55/0, t2 63/0.
Perf 04:05: quiet parity on master 1006c0b8 (scratch/parity_0440.txt): geomean cold 1.03x, warm 0.22x; worst json_01 1.23x, simdjson_02 1.17x, torch_06 1.14x, torch_07 1.13x, rest <= 1.10x - 1.3x target met, 1.1x not. PF4 (lazy candidate-only requests) negative spike, recorded in the p1 parity issue dropped list. PF5 (torch_06/07) no case-specific cost; torch ratios swing ~0.08 run to run (torch_05 1.01x vs 1.09x), recorded too.
Windows master 1006c0b8 (all 9 landings incl. S7 + N50 + PF3) 04:40: test.bat all pass, ex 94/4/42 (SSH GUI), t1 56/0, t2 64/0, 0 DISABLED - TIMEBOX GOAL MET: std tier green on macOS and Windows.
Closed 06:20 at master 50760ad8 (handoff scratch/resume_2026-10-02.md): std goal met; parity 06:15 geomean cold 1.03x, worst json_01 1.21x - 1.3x met, 1.1x not; Luna weekly cap 05:51 (retry 2026-10-06 16:16).
Landed: N67 d96459db..722b4330 06:15 (PARTIAL fix of p2/cpp-by-value-class-temporary-extra-moves: a C++ class rvalue argument crosses into the generated template wrapper as T&& - prvalue into a by-value param 2 -> 1 moves, move x 2 -> 1 (clang 1); guaranteed elision, no-copy/no-move and deleted-move prvalues remain in the issue (wrapper must build the temporary itself); Luna capped -> Sonnet r1, fresh Opus r1 CLEAN; gate 1230/0/8, ex 45/0, t1 50/0, t2 58/0, torch 11/0, cold 127 s / warm 5 s; Win test.bat all pass, ex 94/4/42 (SSH GUI), t1 56/0, t2 64/0). N66 d96459db..d96459db 05:42 (closure capture of a fixed array of string or closure elements is an owning capture: copy/move/destroy per element - master lost string values and crashed on Lambda[2] copies; struct arrays keep master's by-reference capture; Luna r1-r2, fresh Opus r1 FIX FIRST (r1 had switched struct arrays to by-value), r2 CLEAN; leftover p3/closure-move-only-capture-diagnostic-paths (incl. per-element copy unrolled); gate 1230/0/8, ex 45/0, t1 50/0, t2 58/0, torch 11/0, cold 129 s / warm 5 s; Win test.bat all pass, ex 94/4/42 (SSH GUI), t1 56/0, t2 64/0). N65 d96459db..d96459db 05:27 (a bare overloaded function name used as a function-pointer value binds the overload matching the destination signature: call args, C++ fn-pointer params, function<...> init/assign, returns; no match / ambiguous tie refused; C import + hand-written extern of one symbol is one callable; Luna r1, fresh Opus r1 CLEAN; leftover p3/generic-function-name-as-pointer-argument; gate 1230/0/8, ex 45/0, t1 50/0, t2 58/0, torch 11/0, cold 140 s / warm 6 s; Win test.bat all pass, ex 94/4/42 (SSH GUI), t1 56/0, t2 64/0). N64 d96459db..6a823a3d 04:40 (C++ calls rank pointer -> void* above pointer -> bool like clang in free/template/member sets; invalid pointer shapes (non-null void* -> T*, literal -> non-const void* / non-char pointer, width-mismatched pointee by target long) rank below every conversion incl. user-defined and ellipsis, never refused, CFlat fallback kept; a non-null pointer never ranks as a conversion to std::nullptr_t; Luna r1-r3, Opus r1/r2 FIX FIRST, r3 CLEAN; leftovers p3/cpp-pointer-bool-ranking-leftovers (w1 Wrap, multi-arg ill-formed win, el(...) link gap, cxxViable consistency); gate 1228/0/8, ex 45/0, t1 50/0, t2 58/0, torch 11/0, cold 133 s / warm 6 s; Win test.bat all pass, ex 94/4/42 (SSH GUI), t1 56/0, t2 64/0, 0 DISABLED). S7 d96459db..d96459db 04:25 (std tier COMPLETE: std.unique_ptr<int, function<void(int*)>> with a function-pointer deleter; C++ template-argument spelling by what the user wrote (function<R(A)> = R(*)(A), bare R(A) = function type: std.function, packaged_task, Sig templates); T x = T(args) with nested template args built in place, same C++ type with defaulted args written or omitted built in place, overloaded function-name deleter binds the matching overload for selection and lowering; cache 178; Luna r1-r2, Opus r3-r5, Opus r1 / Sol r3 / fresh Opus r4 FIX FIRST, r5 strip per ruling; leftovers p2/cpp-by-value-class-temporary-extra-moves, p2/cpp-template-name-accept-without-converting-ctor, p2/overloaded-function-name-binds-first-overload; rebased over N50 (main session merged err_cpp_capturing_closure_to_cpp legs, closure refusal code taken from N50); gate 1228/0/8, ex 45/0, t1 50/0 0 DISABLED, t2 58/0 0 DISABLED, torch 11/0, std 38/0 0 DISABLED, cold 133 s / warm 5 s; Win (pre-N50 rebase) test.bat all pass, t1 56/0, t2 64/0). PF3 c965eb30 03:55 (clang-generated companion bodies not run through the LLVM verifier, ruling 03:45: VerifyModule checks CFlat functions one by one and skips companion definitions, inline-body modules written unverified; json_01 cold VerifyModule 12.7 -> 0.35 ms; Luna r1, main-session Opus review; gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 130 s / warm 4 s). N50 90a8a8d9..90a8a8d9 03:40 (capturing CFlat closure crosses into std::function and deduced C++ callables through a generated __cflat_closure wrapper, no new syntax: temps / `move f` owned, trivial named locals lent and cloned when C++ moves them; std.function(move f) constructs directly; moving a wrapper moves the env capture by capture (__closure_env_move: scalars copied, owning captures moved, nested closures recursive); unique<T> and by-ref captures refused; core-cache meta 11; Opus r1-r4 (Windows-box run), Sol r1 (filter abort) / r2 / r3 FIX FIRST, fresh Opus r4 CLEAN; leftover p2/closure-capture-owning-array-not-deep-copied (pre-existing); gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 128 s at load ~7 / warm 3 s; Win test.bat all pass, t1 55/0, t2 63/0). N63 90a8a8d9..f53a94d6 03:20 (empty template argument list: C++ X<> names the all-defaulted instantiation (std.barrier<> and bare std.barrier both legal), native VBuf<> with all defaults accepted, Box<> without defaults refused with the existing type-arguments text, <> on a non-generic native or non-template C++ class refused (C++ judged by the recorded request spelling X<...>), empty generic definitions incl. member m<>() refused; Luna r1-r3, Opus r4; Opus reviews r1/r2, Sol r4 FIX FIRST on P2 edge cases -> stripped to p3/cpp-template-empty-argument-list-leftovers; gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 209 s at load ~19 (front-end only change) / warm 6 s). PF2 c965eb30..c965eb30 02:50 (C++ import cold/warm: header content hashes xxh3, request-cache prune once per process per group, first companion blob adopted as the merge module, demand companion generated in the program LLVMContext (no bitcode round-trip); cold instr json_01 -2.9%, simdjson_02 -1.7%, torch_05 -4.0%; parity json_01 1.24x -> 1.22x, all cases <= 1.3x, json_01/json_02/simdjson_02 still > 1.1x - next steps in scratch/briefs/pf2_report.md; cache 177; Sol r1 CLEAN (capacity retry); gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 178 s at load ~29 / warm 6 s; Windows master cfa83fbb: test.bat all pass, ex 94/4/42 (SSH GUI), t1 55/0, t2 63/0). N46 c965eb30..c965eb30 01:55 (a C++ import group includes each header once: an include-only request prelude skips a header clang already included (resolved file identity, any spelling / transitive / earlier prelude); any other directive or a non-exact include line parses the prelude unfiltered; cache 176; Opus r1-r3 + main-session r4 fix, Sol r1/r2/r3 FIX FIRST, r4 CLEAN; gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 163 s / warm 6 s). PF1 c965eb30..c965eb30 01:35 (C++ import cold time: one demand-companion round via a wider up-front body walk (inherited vtable overriders, devirtualized deletes, ctor subobject dtors; union variant members skipped), include-graph reachability cached; torch cold -1.6..-5.2% instructions, train.cb 53.05G -> 52.09G; parity geomean cold 1.07x, warm 0.23x, torch all <= 1.1x, worst json_01 1.27-1.30x; cache 175; Sol r1 FIX FIRST (union dtor) -> main-session fix + leg 2881, Sol r2 CLEAN; gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 172 s at load ~14 (N54 191 s same load) / warm 6 s). N54 90a8a8d9..90a8a8d9 00:58 (std::nullptr_t like clang: nullptr_t returns register, const nullptr_t& params, nullptr deduces decltype(nullptr) in template/free wrappers, nullptr exact for nullptr_t and bool not viable for a null argument at C++ callees, deduction guard only for template-deduction wrappers; cache 174; Opus r1-r3, Sol r2 FIX FIRST, Sol r3 CLEAN; gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, cold 191 s at load ~15 (N62 same window ~187 s) / warm 6 s; leftovers N64). N62 90a8a8d9..90a8a8d9 00:27 (one C++ header through two import groups is one entity when the paths differ in spelling - Windows \ vs /, `..` - compared by file identity, symlink-safe; err_cpp_sink_blame_precise was a log misread, it passes; Win test.bat all pass, t1 55/0, t2 63/0; Mac gate 1228/0/8, ex 45/0, t1 49/0, t2 57/0, torch 11/0, perf void - machine slept during torch_06).
