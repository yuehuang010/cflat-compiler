# Fix queue: ship gate (p1 + p2) first, then the p3/p4 backlog

Rebuilt 2026-10-22 around the ship gate (bucket ruling 2026-10-21: zero p1, every p2 fixed or refused with a located
error). Earlier builds (2026-09-27 C++ interop queue, 2026-09-28 regroup) survive as the Backlog section below.
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

**Maintainer 2026-10-06: both the Mac and the Windows box are in interactive use - no perf / wall-clock measurements on either; gates report pass/fail counts only; libs_perf.sh skipped.** **Maintainer 2026-10-06: tier 3 (torch) is SKIPPED until further notice** - stdlib is the focus; the gate is tiers 1-2
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

## Next timebox: ship gate (p1 + p2), built 2026-10-22

Land all runs below in the next timebox. p1 first (gate: zero). Rows name every issue file they close.
R5 `&v` on a view: refusal is the safe default under the bucket rule; no explicit ruling on `&v` meaning yet.

Tiers: Luna default; Haiku high after a Luna fail; Opus after Haiku. "Win gate" = C++/ABI-touching, needs the Windows box.
Ruling refs: L = fix-issue-lessons.md "Rulings 2026-10-21/22: open-question sweep"; B = "Ruling 2026-10-21: issue bucket meanings" (p2 = fix or located error).

| Id | Item (bucket/stem) | What is wrong (<= 15 words) | Fix (<= 20 words, cite ruling if any) | Area | Size | Tier | Win |
|---|---|---|---|---|---|---|---|
| R7 | p2/statement-temp-escapes-via-out-param-or-global | void*/unrelated pointer joins accepted; C-callee keeps freed temp (use-after-free) | Join refusal landed (T410). Left: C-callee/indirect-fn policy | pointer join conversion, statement-temp release (MainListener_Expressions), C callee policy | M | Luna | yes |
| R9 | p2/unique-field-heap-array-through-move-param | `new T[n]` reaches `move T*`/unique field, freed as one object | RULED 2026-10-25, hold lifted: option 1 - callee-side "adopted into unique<T>" param fact, call-site refusal on precise array provenance; `move T*` keeps the hidden count; also `m = new R()` on a move param releases the old value (see issue head) | param sink check, new T[n] typing (MainListener_Expressions, LLVMBackend) | M | Luna (spike gate) | no |
| R12 | p2/cpp-ternary-default-arm-unbound-callee-wrong-arm | Bulk landed (T418 default/local leaves, T420 field/move leaves). Left: conversion arm into first unbound wrapper call (unknown signature); call arm; (default) parens; ctor const& | See issue "Still wrong"; extend deferral to move/field leaves, register wrapper param type before arg parse | MainListener_PostfixExpression.cpp, ParseTernaryBranches, C++ call-arg state | L | Opus | yes |
| R13 | p2/lowered-cxx-field-structs-in-containers-and-array-fields | (a),(b) landed (T417). Left: R3 arr slot reuse `_ = move arr[0]; arr[0] = b` | (c) R3 element state (needs design). ruled: DESTROY+REBUILD stays | lowered C++ field machinery | M | Opus (after design) | yes |
| R14 | p2/cpp-forwarded-fnptr-variable-or-closure-dangles-in-wrapper-frame | fn-ptr var or closure forwarded to C++ T&& dangles in wrapper frame | Blob-record family landed (T419: std-owner rule, MSVC shared_ptr). Left: see issue (std::function(std::ref(f)), caller-frame materialization, BorrowDtor/Mixed blind spots) | C++ wrapper/forwarding (CInterop, LLVMBackend_Overloads) | L | Opus | yes |
| R16 | p2/delete-borrow-via-named-local + p2/view-mixed-join-sunk-through-pointer-param-alias | Owner/borrow reaches sink via helper chain or ptr param; double free / UAF | One interprocedural callee summary ("param stored into borrowing container / sinks view field"); path-sensitive provenance. Parked fix/t396-viewalias; not same-function guard (digest) | callee summaries, MemoryOutlivesCall walk, T387 view state | L | Opus | no |
| R17 | p2/iface-return-dangle-via-calls-aggregates-and-rebinds | Returned interface boxes pointer from local via call/heap/indirect write | RULED 2026-10-25: Rust lifetime elision - call result borrows from every pointer arg + receiver unless callee summary proves fresh/global/param N; heap aggregate alias refused the same way (see issue head) | provenance through memory; PointerValueRoot | L | Opus | no |

### Conflict map (never concurrent)
- R1, R6, R14: function-value / callable binding (ParseTernaryBranches + function<> sinks). R6 before R14.
- R6, R7, R11, R12, R13, R14: C++ call args, LLVMBackend_Overloads, CInterop. R11 -> R12; R13 not with R6/R14.
- R3, R9, R10: param consume/ownership checks (WinRT call-site + move sink rules).
- R4, R15: alias-return gate / alias closure ABI. R4 first.
- R5, R8, R9, R16, R17: MainListener_Expressions provenance / view state; R16 after R5 and R8 (same T382/T387 keys).
- R2 with R15, R1: lambda capture and copy paths.

### Parallel waves (max 3 implementers)
- Wave 1: R1, R2 (long, Opus), R3
- Wave 2: R4, R5, R6
- Wave 3: R7, R8, R10 (R10 sub-run a/b only)
- Wave 4: R9, R11, R13
- Wave 5: R12, R15, R16
- Wave 6: R14 (R17 only after a ruling)
Windows gate: R6, R7, R11, R12, R13, R14 land after the Windows box passes. R2 is the long pole; start it in wave 1 and let waves 2-3 refill the other 2 slots as they finish.

### Not ready
- R17 iface-return-dangle: RULED 2026-10-25 (elision rule, see issue head); heap-aggregate alias and indirect-write sub-items tried 3x by Luna - Opus.
- p2/string-copy-through-pointer-subscript global-pointer and implicit-this parts: need a runtime provenance design or ruling (compile-time global flag depended on emission order); Luna failed 3 attempts.
- R13(c) R3 `_ = move arr[0]; arr[0] = b`: element state not static; needs design.
- R10(d) conditional moved-source read: path-sensitive moved-state design, no plan yet.
- R16 and R17 share one needed plan: callee provenance summaries (no internal/plan entry exists); R16 may start only as design spike. T396 refusal of `if (owned) sink(move p->f)` is kept (L).
- R5: no explicit ruling on `&v` meaning (element address vs refusal); refusal is the safe default under B, flag to maintainer in the brief.


## In flight

| Run | Branch | Issue(s) | Notes |
|-----|--------|----------|-------|
| P3 | - | p3/cpp-unique-field-move-from-field-refused | RULED 2026-10-24: accept field-path move source `h.w = move g.w` (C++ row: Windows gate) |
| P3 | - | p3/cpp-ctor-address-of-pointer-spelling-vs-clang | RULED 2026-10-24: `&p` = T** value when any candidate accepts it; slot binding fallback only (C++ row) |
| P3 | - | p3/cpp-ctor-ranking-leftovers (item 3) | RULED 2026-10-24: no float->int at C++ ctor args in any form incl. reference params (C++ row) |
| P3 | - | p3/cpp-pointer-return-unrequested-specialization-not-retried | RULED 2026-10-24: no `a @= b` -> `a = a @ b` fallback on C++ classes (C++ row) |
| P3 | - | p3/named-brace-init-partial-unwind (item 1) | RULED 2026-10-24: keep refusal, reword diagnostic only |
| P2 | - | p2/unique-array-element-assign-borrow-double-free | filed T486 review 2026-10-24: unique array-element store has no borrow check (double free); route through T486 check |
| P3 | - | p3/moved-from-struct-leftovers-after-t490 | filed 2026-10-24: reassign-after-move extra dtor, T418 C++ param `_ = move` (immediate release + disarm), field move, raw-pointer-field scope ruling question |
| P3 | - | p3/local-fixed-array-elements-destroyed-forward | filed T488 review: local `T[3]` dtors forward, C++ reverse; reuse T488 helper |
| P3 | - | p3/nested-function-type-parameter-not-compared | filed T427 review: function-type component of a function type not compared |
| P3 | - | p3/call-name-shadowing-leftovers-after-t484 | filed T484 review |
| P3 | - | p3/function-pointer-double-pointer-address-of-named-function-accepted | filed T478 |
| P3 | - | p3/nested-type-lookup-leftovers-after-t481 | filed T481 |
| P3 | - | p3/returned-pointer-from-call-result-field-temp-dangles | T510 residue: RULED 2026-10-25 item 1 REFUSE (mixed-select legs -> err_); raw temp field into unique int* param double free, ternary owner, accepted leaks |
| P3 | - | p3/ctor-lambda-this-closure-relocation | T515 residue: by-value return of a this-capturing-closure holder awaits the native return-ABI ruling |
| P3 | - | p3/self-pointer-struct-return-not-in-place | RULED 2026-10-25: C++ guaranteed elision for every ctor-/dtor-bearing native struct (sret + in-place new); plan first |
| P3 | - | p3/crossed-address-of-state-leaks-call-temporary | T485 leftovers: C2 non-storing callee leak, two-owner pointer difference, pre-existing store3 EARLY FREE |
| P3 | fix/t422-globdef | p3/global-default-spelling-zeroes-without-the-note | RULED 2026-10-25: native globals Rust-strict, C++ per clang; land r2 + rewrite the 3 sites (see issue head) |
| P3 | fix/t438-uniqdot | p3/cflat-unique-dot-dispatches-to-pointee-member | RULED 2026-10-25: own-first strict hiding, `.` == `->`, drop the shape fallback (see issue head) (T438) |
| R16 (parked) | fix/t396-viewalias | p2/view-mixed-join-sunk-through-pointer-param-alias | Sol r2b: 4 INCOMPLETE, needs path-sensitive provenance |

## Backlog (p3/p4 and plans; groups from earlier builds, unchanged)

Rows that named only p1/p2 issues moved to the ship-gate section above and were dropped here.

### A - crashes, asserts, link failures

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| A3 | p3/cpp-incremental-retry-after-failed-parse-crashes | ModuleBuilder / IncrementalAction GenModule | latent (t14-t16, t31 cold retries); known trigger closed. A1 landed 2026-09-29; run after TOPSTMT (same incremental action) |

### B - wrong value / wrong overload pick

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| - | p3/cpp-using-declaration-member-access-gaps | C++ record flattening + member lookup | `using` re-exposure through a base, two same-named base fields, shadow + using, method using, virtual base: refused, clang accepts; T99 reviews 2-4, pre-existing |
| - | p3/cpp-macro-constant-leftovers-after-t98 | macro identity + brace fold | global/function name collisions, anonymous/inline-namespace enums, comma, `auto&&`, deref macros, C errno/INT_MAX; T98 reviews 1-2, pre-existing |
| - | p3/cpp-ctor-template-overload-and-copyinit-gaps | C++ ctor-template ranking | unnamed-enum source picks int ctor; non-const trivially-copyable lvalue copy-init skips U&& template (T54 review, pre-existing) |
| - | p3/cxx-byvalue-prvalue-arg-moved-not-elided | prvalue C++ class into a by-value param | extra move vs clang elision; the old p2 by-value-temporary-extra-moves file was gone, re-pointed 2026-10-15 (T229) |
| - | p3/cpp-reference-member-leftovers-after-t47 | ref members | designated init refused; const array-ref member write accepted; const C& result ranks C& first (T47 reviews, pre-existing) |
| - | p3/function-ptr-c-abi-leftovers-after-t77 | function<> C ABI | short callback return at -O2; extern `move T*` count lost (T77 review 2, pre-existing) |
| - | p3/cpp-ctor-template-viability-not-clang-selected | C++ ctor narrowing | template constraints / failed deduction not seen; validate clang-selected ctor (T78 reviews 2-5) |
| - | p3/auto-local-of-arithmetic-takes-cflat-width-type-at-cpp-calls | auto local at C++ template call | `auto x = 1e3 + 1; fwd(x)` float / refused, `auto x = l + 1` long long; T58 review 5, pre-existing |
| - | p3/simd-float-postfix-increment-generic-message | simd<float,4>++ | generic lane-count error; decide lane-wise or specific refusal; T71 review 1 |
| - | p3/cpp-wrapper-request-misses-template-argument-headers | `std.unique_ptr<cppon.Cls>` wrappers | importer-only header group; hidden by incremental; T70 round 2 |
| B5 | p3/cpp-const-twin-overloads-collapse | C++ class member registration (const / non-const twins share one CFlat signature) | NEW 2026-09-28 (B1 matrix, OUT cells C1/C4, E1-E5, F1/F4, H1/H4/H9, L10) |
| B5b | p3/cpp-constrained-twin-default-argument-ranking | C++ class member registration (constrained same-signature twins with a default argument share one CFlat signature) | NEW 2026-10-03 (T22 review rounds 2-4; pre-existing on master) |
| B5g | p3/cpp-callback-pointee-const-ranking-gaps | function-pointer values lose pointee const; clang-refused callback sets accepted | NEW 2026-10-03 (T25 rounds 3-5, pre-existing on master) |
| B5h | p3/cpp-nested-member-class-free-return-and-call-result-assign | free function returning a nested member class; assignment to a class result of operator()/operator* | NEW 2026-10-03 (T30 review 2, pre-existing on master) |
| B5j | p3/cpp-pointer-ref-overload-ctor-and-direct-member-ranking | `const char *const &` overloads: constructor wrapper path and direct non-template ranking; literal at a `const char *&&` template refused | NEW 2026-10-05 (T28 round 5 matrix, pre-existing on master) |
| B14 | p3/c-flexible-array-member-leftovers | raw interior-pointer `move` (RULING: explicit move of a raw pointer), implicit-this message, 3 pre-existing P3 | OPEN (array-member move refused T489) |
| B16 | p3/cpp-operator-move-operand-leftovers | template U&&, elision, cond move into forwarding operator=, member-receiver lhs move | NEW 2026-09-28 (D8 report) |
| B18 | p3/cpp-assignment-result-leftovers-after-b9 | extra copies (paren call arms, arr elem init) + bare scalar/nested ternary refusals + 3 unrelated | OPEN (P2 landed 1955bd95) |
| W13b | p3/cpp-overload-variadic-and-conversion-gaps | variadic-set ambiguity, defaulted param before ellipsis, member variadics, user conversions at C++ call args, inline variadic link, non-record const pointers | NEW 2026-09-30 (W13 reviews) |
| G6 | p3/header-cache-residual-growth | in-version prune age rule (sigbase, dead request configs, cxxdemand) | NEW 2026-09-29 (split from G5) |
| B2 | PARKED 2026-09-28 after 3 Codex rounds (scratch/b2_parked.patch): p3/cpp-string-literal-template-deduction-and-unspellable-char-args + p3/cpp-std-min-long-and-pointer-arguments-refused. Free-array issue closed (premise disproved, [over.ics.rank]/3.2.1). Restart from master on opus with the narrower design in the issue files | RequestCxxFunctionTemplate argument spelling (CxxStringLiteralSpelling, InferImplicitCxxArgumentType) | NEW. One mechanism: spell array / literal lvalues as `*reinterpret_cast<E (*)[N]>(p)` like the member path (e6c60220); give `long` arithmetic, `char*` and pointer-arithmetic rvalues a C++ spelling |
| B3 | p3/cpp-single-level-pointer-unproven-at-call-argument | IsProvenPrimitiveSinglePointerArg / CompareUpconvert | old bucket 2b (2a landed as AT). Pointer-is-not-a-number ruling |

### C - wrongly accepted (adds a rejection)

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| - | p3/cpp-operator-template-set-ambiguity-without-drop-accepted | C++ operator ranking | free-only ambiguous operator template set with no dropped candidate accepted (clang ambiguous); int*/void*/template no-drop case (T85 reviews 3-4, pre-existing) |

### D - wrongly refused (acceptance gaps)

| Run | Issue(s) | Site | Mode |
|-----|----------|------|------|
| - | p3/cpp-static-local-derived-to-base-and-lvalue-ternary-copies | static-local C++ init | derived-to-base static refused with array-decay text; lvalue-arm ternary static skips the copy ctor; temp dtor exception not propagated (T93 review 1, pre-existing) |
| - | p3/cpp-null-macro-and-nullptr-spellings-refused | null-constant classifier + converting ctor | full; NULL/__null/nullptr macros, nullptr -> std::function (T52 review, pre-existing; AFTER T52) |
| D1b | p3/cpp-unguarded-header-class-template-lookup-fails-cold (cold-cache only) | free-function selection; incremental request units | NEW 2026-09-28 (D1 leftovers) |
| - | p3/cpp-expression-template-to-cflat-byvalue-class-param-refused (NEW) | converting-ctor path | absorbed by P1 (converting constructors); land there |

### E - temporary lifetime, leak, dangle (one issue per run, opus-tier area)

| Run | Issue | Notes |
|-----|-------|-------|
| E3 | p3/cpp-unwind-cleanup-gaps-ctor-new-temporaries | follow-up list after d59df39b / cpp-unwind2 |
| E4 | p3/cpp-new-remaining-allocator-gaps | follow-up list after cpp-class-operator-new (5 items) |

### F - diagnostics only

| Run | Issue(s) | Mode |
|-----|----------|------|
| F1 | p4/localize-relayed-clang-diagnostics + p3/cpp-sink-refusal-cause-lost-under-harvest-suppression | one run: every relayed clang cause surfaces, prefixed `clang: ` (2026-09-27 ruling, no translation). Batch for the prefix sites; the harvest-suppression half is full-mode review |

### G - request cache and incremental infrastructure

| Run | Issue(s) | Mode | Notes |
|-----|----------|------|-------|
| G2 | p3/cpp-wrapper-request-group-omits-type-dependencies + p3/cpp-candidate-tier-differs-cold-vs-warm | batch | RequestGeneratedCxxWrapperUncached, CandidateCxxGroupsFor. After G1 |
| G3 | p3/cpp-check-mode-cannot-repopulate-type-request-cache | full | design: --check writing the cache |
| G4 | p3/cpp-noinc-mode-std-map-lookup-fails | full | root cause not established; investigation first |

### H - coverage only (no known defect)

| Run | Issue | Notes |
|-----|-------|-------|
| H1 | expression-template-ctor half of p3/cpp-inherited-members-and-expr-ctor-coverage-gaps | `requires` / `enable_if` ctor templates, copy/move/dtor counts, by-value `operator=`, same-named records in two namespaces. Cheap; delete the issue when A2 + H1 both landed |

### P - plans and staged work

| Id | Item | Notes |
|----|------|-------|
| P1 | plan converting-constructors (`internal/plan/converting-constructors.md`) | ruled 2026-09-26; eigen_05; absorbs D-row converting-ctor-raw-array + expression-template-to-cflat-byvalue-param. Largest item, own staging |
| P2 | plan cflat-struct-nontrivial-cxx-fields (`internal/plan/cflat-struct-nontrivial-cxx-fields.md`) | ruled A 2026-09-27. Phase 0 (stopgap refusal) is small and can go early; phases 1-4 one per branch. Absorbs p3/cpp-typed-local-extra-copy-from-param-and-ctor-arms |
| P3 | p3/cpp-std-header-survey-refresh | re-run the std header L0/L1 survey on macOS and Windows to refresh the gap list; untouched headers need a CFlat-surface ruling |
| P5 | p3/debug-assert-setvisibility-cross-target-elf | cross-target (-p win64) only; fold into p3/cross-target-compile-gaps work |
| P4 | virtual-base layout (item 2 of p3/cpp-inherited-member-access-and-virtual-base-diagnostics) | refused today ("layout cannot be reproduced", CInterop ~14229); needs a plan |

### New or rewritten 2026-09-29 (leftovers from landed rows; group at the next queue build)

| Run | Issue | Summary | Status |
|---|---|---|---|
| N3 | p3/cpp-const-object-nonconst-member-call-writes-readonly | loop back-edge const-pointer dataflow, virtual-base twin | W13 landed const auto copy, const globals, const-pointer overload selection 2026-09-30; rest open |
| N9 | p3/cpp-assign-operator-route-leftovers | C++ assignment operator route leftovers | NEW 2026-09-29 |
| N10 | p3/cpp-brace-argument-backing-remaining-shapes | Brace-list backing: optional constructor parameter case remains | NEW 2026-09-29 |
| N12 | p3/cpp-conversion-operator-receiver-leftovers | C++ conversion-operator receiver and class-result leftovers | NEW 2026-09-29 |
| N13 | p3/cpp-ctor-expression-position-const-ref-copies-lvalue | C++ ctor in expression position - remaining `const T&` copies and enum `T&&` refusal | NEW 2026-09-29 |
| N14 | p3/cpp-ctor-ranking-leftovers | P3 C++ constructor ranking leftovers after C6 round 2 | NEW 2026-09-29 |
| N15 | p3/cpp-default-wrapper-batch-chunk-stores-stripped-prefix | C++ header default-wrapper batch chunk stores a stripped prefix (latent replay failure) | NEW 2026-09-29 |
| N18 | p3/cpp-pointer-return-unrequested-specialization-not-retried | Remaining C++ free-operator and converting-constructor retry gaps | NEW 2026-09-29 |
| N22 | p3/cpp-template-ctor-deduction-enum-and-arith-rvalue | C++ template constructor deduction - enum lvalue and arithmetic rvalue deduce the wrong T | NEW 2026-09-29 |
| N23 | p3/cpp-unary-operator-leftovers-after-d7 | Unary operator leftovers after D7+C3 | NEW 2026-09-29 |
| N24 | p3/cpp-unrequested-specialization-pointer-eager-request | Bucket: p3 (C++ interop compile time; left by D6, fix/unrequested-spec-pointer-retry, 2026-09-29) | NEW 2026-09-29 |
| N28 | p3/cpp-deferred-special-members-mid-size-harvest-cost | R1 deferral costs +1-2% cold on 64..~500-record harvests (interop fixtures); cutoff adjustable later (maintainer) | NEW 2026-09-29 perf |
| N31 | p3/cpp-interpreter-drops-cc1-only-flags | -Xclang cc1-only flags silently dropped by the Interpreter driver (pre-existing, H1 review) | NEW 2026-09-29 perf |
| N34 | p3/cpp-deployment-target-env-core-cache-miss | Non-default MACOSX_DEPLOYMENT_TARGET misses the core bitcode cache every compile (R4 review) | NEW 2026-09-29 perf |
| N35 | p3/cpp-demand-bodies-torch-cold-cost | R4 costs +4.4% cold instructions on torch (json -28%); verdict writes batched in 93b361f6 (-0.55%), rest is clang instantiation | PART 2026-09-29d |
| N37 | p3/cpp-signature-registration-projects-records | Registration projects every record a signature names; -3.6% torch only by skipping projection, which breaks overload order (D4) | NEW 2026-09-30 perf |
| N41 | p3/cpp-variadic-alias-template-refused | in-repo `template<auto... I> using A = S<I...>` refused (alias machinery has no pack support) (ST4, pre-existing) | NEW 2026-10-01 |
| N42 | p3/cpp-header-unreferenced-global-initializers-skipped | unreferenced C++ header inline globals never run their dynamic initializer (referenced ones + .cpp sources fixed/fine, T44) | RULED 2026-10-25: follow clang, run every harvested header global initializer, budget-gated; p3 |
| N47 | p3/cpp-constant-fold-and-const-static-twin-leftovers | constexpr-call initializer refused by the fold guard; const receiver + static twin overload refused (ST4 re-review leftovers) | NEW 2026-10-01 |
| N52 | p3/cpp-range-for-member-name-lookup-and-count-probe | range-for member lookup misses enum/nested-type begin/end (accepts invalid); count/get clients pay a failed ADL probe; ST6 follow-up | NEW 2026-10-01 |
| N58 | p3/cpp-struct-identity-underscore-namespace | [cpp] identity: ns_ namespace decode, global name collision, CRTP base reading a derived field (master too); N51 follow-up | NEW 2026-10-01 |

### std smoke suite gaps 2026-10-01 (test_libs/std DISABLED cases; group at the next queue build)

`DISABLED_WIN` / `DISABLED_MAC` disable a case on one platform only (MSVC-only / libc++-only gaps). Fixing a row removes the DISABLED marker of its case(s) in the same commit (`./test_libs.sh std --include-disabled` shows XPASS).

| Run | Issue | DISABLED case | Summary |
|---|---|---|---|

### std_full tier-2 gaps 2026-10-02 (test_libs/std_full DISABLED cases; plan internal/plan/std-library-tier2-suite.md)

`DISABLED_WIN` / `DISABLED_MAC` disable a case on one platform only (MSVC-only / libc++-only gaps). Fixing a row removes the DISABLED marker of its case(s) in the same commit (`./test_libs.sh -t 2 std_full --include-disabled` shows XPASS). Wrong-code / crash rows first: valarray literal operand, valarray ptr ctor, optional<string> deref compare, errc conversion, insert_iterator copy, wcs* recursion, char[N] verifier failure, noop_coroutine LLVM fatal.

| Issue | DISABLED case | Summary |
|---|---|---|
| p3/cpp-reference-param-conversion-gaps-after-t42 | (C++ interop) | template ctor from non-template source, inherited operator X(), operator X&/X&& into C++ ref params refused (clang accepts; T42 follow-up) |
| p3/cpp-pointer-depth-and-array-pointer-param-gaps | (C++ interop) | non-null int** accepted into C++ int***; member/static array-pointer params refuse valid args |
| p3/float-literal-plus-variable-computed-in-float | (C++ interop) | `i + 1.0` / `f + 1.0` typed float (clang double); check CFlat literal rule first |
| p3/generic-overload-not-joining-concrete-overload-set | (none) | generic f<T>(T*) never joins concrete overloads; 12 clang-valid calls refused |
| p3/member-type-path-remaining-gaps | (none) | `Outer<int>.Inner<long>.type`, closure-alias members, CFlat static data members, C++ statics as value template args (T37 follow-up) |

### Ruled 2026-10-01 - ready to schedule (rulings recorded in each issue file)

| Ruling | Issue | Work |
|---|---|---|
| Q2 | p3/alias-return-byvalue-param-leftovers-after-b21 | item 5 ratified (extern C callee owns by-value params) - no code |
| Q9 | p4/three-way-comparison-operator | `<=>` in the CFlat language RULED 2026-10-02, plus defaulted member-wise `==` - buildable (result type without <compare> follows C++ unless told otherwise) |

### Ruled 2026-10-09 - ready to schedule (rulings recorded in each issue file)

| Issue | Ruling |
|---|---|

### Grouped runs 2026-10-10 (ranked; source scratch/issue_groups_2026-10-10.md)

Regrouping of every open p1-p3 issue not in flight, parked, plan-owned or ruling-gated. Score = value / (complexity x risk),
value 1.5 per p2 + 1 per p3; C = S/M/L, R = L/M/H (H = C++ overload / CInterop wrappers / request cache / ownership, or torch).
Cheap = fits a Haiku implementer (anchored, risk L-M). Sites marked (u) come from issue text only - confirm by grep before the run.
The A-H sections above stay the per-issue record; schedule from this table.

| # | Run | C/R | Issues | Shared site / root cause | Not concurrent with | Cheap |
|---|-----|-----|--------|--------------------------|---------------------|-------|
| 1 | DIAG-TEXT | M/L | p3/cpp-sink-refusal-cause-lost-under-harvest-suppression | shared KIND only (relayed clang / refusal text); no shared function (u) - may split into 2 runs | F1 | yes |
| 2 | STATIC-INIT | M/M | p3/global-default-spelling-zeroes-without-the-note | MainListener_Declarations.cpp static/global init arms (EmitCxxStructorCall ~6475, GenerateDefaultValue global arm ~1620) | - | yes |
| 3 | NULLCTOR | M/H | p3/cpp-null-macro-and-nullptr-spellings-refused | C++ ctor null-constant selection: scalar-reference mirror (CInterop ~15375), SelectCxxConstructorListed (~26846), RequestCxxBraceConstructor | NULLPTR-T, CTOR-VIABILITY, PTRRANK, CTOR-BRACE, CONSTMACRO | no |
| 4 | NSLOOKUP | M/M | p3/cpp-struct-identity-underscore-namespace | CheckCxxNamespaceConflicts + struct identity canonicalization (CInterop) | - | yes, careful on identity |
| 5 | OPUNARY | M/M | p3/cpp-unary-operator-leftovers-after-d7 | unary `*` in ParseUnaryExpression (MainListener_PostfixExpression.cpp) | D2/D3 operator dispatch, CONVOP | yes |
| 6 | MOVEPARAM | M/H | p2/byvalue-param-owning-leftovers-after-b13, p2/unique-field-heap-array-through-move-param | move / by-value owning param at the callee boundary (u) | NEWTEMP, BORROW-PROV | no (Opus reviewer) |
| 8 | PTRRANK | M/H | p3/cpp-single-level-pointer-unproven-at-call-argument, p3/cpp-pointer-depth-and-array-pointer-param-gaps | CompareUpconvert (Overloads), IsProvenPrimitiveSinglePointerArg (CInterop), TryBinaryOperatorOverload (supersedes old B3 row) | T89 in flight (same predicate) - after it lands | no |
| 9 | FIELDCTOR | M/M | p3/cpp-class-field-default-construction-leftovers-after-v20 | field initializer lowering, MainListener_Aggregates.cpp (u) | - | yes |
| 10 | CLIBNAME | M/M | p3/core-defines-externally-visible-libc-functions | core defines libc names that LLVM folds as builtins | RULING first (nobuiltin on CFlat bodies) | after ruling RULED 2026-10-21: nobuiltin on CFlat bodies (see lessons digest). |
| 14 | CONVOP | M/H | p3/cpp-conversion-operator-receiver-leftovers | C++ conversion-operator call path (u) | D2/D3, OPUNARY | no |
| 15 | ASSIGN | M/H | p3/cpp-assign-operator-route-leftovers, p3/cpp-assignment-result-leftovers-after-b9, p3/cpp-nested-member-class-free-return-and-call-result-assign | C++ assignment route + class-result receivers (u) | T92 in flight (operator=(T)) | no |
| 16 | REFPARAM | M/H | p3/cpp-reference-param-conversion-gaps-after-t42, p3/cpp-reference-member-leftovers-after-t47 | conversions into C++ reference params (u) | REFBIND pair, CTOR-VIABILITY | no |
| 18 | CTOR-VIABILITY | L/H | p3/cpp-ctor-template-viability-not-clang-selected, p3/cpp-ctor-ranking-leftovers, p3/cpp-ctor-template-overload-and-copyinit-gaps, p3/cpp-pointer-ref-overload-ctor-and-direct-member-ranking | CxxCtorSetRequiresNarrowing + ctor wrappers | NULLCTOR, REFPARAM | no |
| 20 | FNWRAP | L/H | p2/cpp-forwarded-fnptr-variable-or-closure-dangles-in-wrapper-frame, p3/cpp-capturing-closure-to-std-function | wrapper-frame slot for fnptr/closure into C++ T&& (RequestCxxFunctionTemplate) | ELIDE (P2 plan) | no |
| 22 | CHUNK | M/H | p3/cpp-demand-chunk-order-numbering-per-interpreter, p3/cpp-default-wrapper-batch-chunk-stores-stripped-prefix | chunk store, CxxIncrementalGroup ParseRequest | CACHE-KEY, TOPSTMT | no |
| 25 | CONSTTWIN | M/H | p3/cpp-const-twin-overloads-collapse, p3/cpp-constrained-twin-default-argument-ranking | RegisterCxxClassMembers (~22175) dedupe keeps the non-const twin | CONSTPOINTEE pair | no |
| 26 | SRCTYPE | L/H | p3/auto-local-of-arithmetic-takes-cflat-width-type-at-cpp-calls, p3/cpp-template-ctor-deduction-enum-and-arith-rvalue | source type not propagated into C++ call construction (CxxArithIdentity) | T92 in flight - after it lands | no |
| 27 | CACHE-KEY | L/H | p3/cpp-wrapper-request-group-omits-type-dependencies, p3/cpp-wrapper-request-misses-template-argument-headers, p3/cpp-candidate-tier-differs-cold-vs-warm | RequestGeneratedCxxWrapperUncached, CandidateCxxGroupsFor; partial patch scratch/repro_keep/t70_groupfix.patch | G2, CACHE-VALID | no |
| 28 | BORROW-PROV | L/H | p2/delete-borrow-via-named-local, p3/consolidate-named-variable-borrow-provenance | borrow provenance of named variables through helpers (silent UAF) | MOVEPARAM; design first | no |

Pairs (take together when scheduled): CONSTPOINTEE = p3/cpp-callback-pointee-const-ranking-gaps (typed-null half closed by C05B);
CASTWRAP = p3/cpp-const-char-pointer-explicit-class-cast-refused + p3/cpp-fixed-char-array-explicit-class-cast-refused;
REFBIND = p2/cpp-pointer-reference-ctor-param-binds-temporary + p3/cpp-ternary-of-pointer-lvalues-at-reference-param-binds-copy (same symptom, different sites - one run, two fixes);
VIEWJOIN = p3/owning-view-ternary-mixed-arm-flag + p3/raw-pointer-join-into-array-view-param-accepted.
Cheap singletons (S, risk L): p3/test-err-discover-command-line-overflow, p3/os-abstraction-process-cb-not-converted, p3/native-brace-init-no-narrowing-check,
p3/pointer-to-number-diagnostic-hint-leftovers, p3/view-element-gate-diagnostic-gaps. Full singleton list with C/R: scratch/issue_groups_2026-10-10.md section 2.
Overload/ctor selection family (never two at once): NULLCTOR, NULLPTR-T, PTRRANK, CTOR-VIABILITY, CTOR-BRACE, CONSTTWIN, CONSTPOINTEE.
Stale references found by the regroup: the std_full table names p3/static-auto-prvalue-and-copy-initializer-storage, which has no issue file (fixed or never filed - verify);
A3's "after A1" condition is met (A1 landed 2026-09-29).

### Parked - needs a maintainer ruling before any work

| Issue | Question |
|-------|----------|
| p3/cflat-unique-dot-dispatches-to-pointee-member | `.` on CFlat unique<T>: prefer unique's own members (like C++ smart pointers after T46) or keep forwarding? RULED 2026-10-21 (see lessons digest). |
| p4/string-functional-construction-spelling | p4: RULED 2026-10-21 - string(ptr) and string(ptr, len) borrow (see lessons digest) |

### Conflict map (never concurrent)

B3, C1, B4, D1 all touch LLVMBackend_Overloads ctor/argument selection - run them one after another
(order C1 -> B4 -> B3 -> D1, or D1 first as the cheap batch). D2 and D3 share operator dispatch. A1 and
A3 share the incremental action. G1 -> G2 -> G3 share the request-cache key. E1 touches
RequestCxxBraceConstructor, which B2's template spelling calls into - not concurrent with B2.
