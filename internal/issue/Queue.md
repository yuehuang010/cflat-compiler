# Fix queue: ship gate (p1 + p2) first, then the p3/p4 backlog

Rebuilt 2026-10-22 around the ship gate (bucket ruling 2026-10-21: zero p1, every p2 fixed or refused with a located
error). Earlier builds (2026-09-27 C++ interop queue, 2026-09-28 regroup) survive as the Backlog section below.
Refreshed 2026-11-02 after box 9: rows of closed issues dropped, every open issue file listed (native issues in their
own sections). Workflow is `internal/skill/fix-issue/SKILL.md` (full mode by default, batch mode only where marked).
The main session owns this file: agents never edit it (see the worktree-agents rule). A landed row is removed (git log
carries its hash), and its issue file is deleted in the fix commit.

## Landing gate (every row)

Green on the rebased branch, run by the main session before `git merge --ff-only`:

```bash
./cmake_build.sh release && x64/Release/cflat --init-local
bash test.sh Release -j 4          # 0 failed
bash test_example.sh               # 0 failed
./test_libs.sh -j 2                # tier 1 (incl. eigen, std_full): 0 FAIL; an XPASS means a DISABLED marker comes off in the same commit
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

Timing note (2026-11-02): a build-stamp change over a full main-checkout local cache inflates buildci ~2x
(PruneCxxTypeRequestDiskCache scan, p2/cxx-request-cache-prune-scan-doubles-buildci); clear the local cache
before a timed run.

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
| N | native ownership / move / borrow | full, opus-tier area | ctor/dtor counts, false-refusal sweep over Test/ |
| L | native language surface (types, generics, tuples) | full | value + diagnostic, generic and non-generic spellings |
| P | plans and large staged work | plan | one phase per branch |

A **run** is one worktree + one branch. Issues that share a function go in the same run (never two
concurrent runs on the same function). Line numbers in old rows predate the 2026-10-31 move-only splits
(LLVMBackend_CInterop.cpp -> seven TUs, MainListener_Expressions.cpp -> six TUs); grep the function name.

## Ship gate (p1 + p2)

Empty: p1 = 0, p2 = 0 (box 9 closed green 2026-11-02, master 16102f79, pushed). Run the Windows gate on the master tip
once per box.

Open: p2/expect-error-substring-source-check-loose (filed 2026-11-07, test-infra: the leg lint's check 2
accepts nearly any text); p2/cxx-request-cache-prune-scan-doubles-buildci (filed
2026-11-07, compile time: narrowed to one directory scan per storing process, ~25% over cold);
p2/cold-cpp-interop-compile-peaks-23-27-gb (filed 2026-11-07, compile memory: 23-27 GB per cold
test_cpp_interop compile; pages the Windows box).

## In flight

None. Parked branches (worktrees removed 2026-11-03, branch kept, not landing as is): `fix/t345-ctorlambda`
(p3/ctor-lambda-this-closure-relocation), `fix/t348-autoconst` (p3/native-auto-copy-drops-transitive-const),
`fix/t396-viewalias` (mixed owned/borrowed view field moved through an alias). Rebase or drop before reuse.

## Partly landed - residue open

| Issue | Landed | Open |
|-------|--------|------|
| p3/moved-from-struct-leftovers-after-t490 | MVFIELD (af5834ad3), MOVEDTOR2 (value dtors always run on the zero-filled source, ruling 2026-10-31) | callee destroys by-value params always (RULED 2026-10-29, CFlat ABI; T499 attempt backed out), conditional consumes need the callee drop flag |
| p3/interface-call-overload-ranking-and-conversion | IFACEOVL (483f3b83d), IFCONST (5344802ed) | native ConstMask hole (needs core const annotations), first-not-ambiguous picks, interface single-slot widen, native pointer-field const carry, const twin mangling (all pre-existing) |
| p3/ctor-lambda-this-closure-relocation | T515 | by-value return of a this-capturing-closure holder awaits the native return-ABI ruling; branch fix/t345-ctorlambda |
| p3/self-pointer-struct-return-not-in-place | - | RULED 2026-10-25: C++ guaranteed elision for every ctor-/dtor-bearing native struct (sret + in-place new); plan first |
| p3/cpp-mixed-class-conditional-value-return-argument-refused | MIXCOND1-3 (8844053c8), MIXS2 (S2 re-admitted) | S3 sret copy, lv_bv, nested mixed, CFlat/C++ temp order, win64 cross-compile refusal |

## Backlog (p3/p4 and plans; groups from earlier builds)

### A - crashes, asserts, link failures

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| A3 | p3/cpp-incremental-retry-after-failed-parse-crashes | ModuleBuilder / IncrementalAction GenModule | latent (t14-t16, t31 cold retries); known trigger closed |
| A4 | p3/cpp-tu-odr-split-intermittent-base-lost | [cpp] struct TU emission | intermittent test_cpp_interop rc, not reproduced; evidence in the file |
| A5 | p3/debug-assert-sweep-leftovers | Debug (assertions-on) builds | residue of the 2026-11-01 Debug sweep |
| A6 | p3/windows-grouped-err-check-clang-layout-stack-overflow + p3/msvc-postfix-expression-frame-965kb | Windows stack depth | MSVC 965 KB ParsePostfixExpressionInner frame; grouped --check overflow in clang layout |
| A7 | p3/run-cxx-runtime-link-and-exit-gaps | --run C++ runtime | Windows link failures, macOS exit() teardown after WINRUN; RULED 2026-11-04: two new/delete families now, one-family plan P7 |

### B - wrong value / wrong overload pick

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| - | p3/cpp-using-declaration-member-access-gaps | virtual base re-exposed by `using` (USINGDECL af28ab81 landed the rest) | with P4 virtual-base layout |
| - | p3/cpp-macro-constant-leftovers-after-t98 | macro identity + brace fold | residue after MACROLEFT (af28ab81): `auto&&`, anonymous-enum lvalue deduction / joins, comma lvalue category |
| - | p3/cpp-ctor-template-overload-and-copyinit-gaps | C++ ctor-template ranking | non-const trivially-copyable lvalue copy-init skips U&& template (unnamed-enum half landed 2026-10-30; CTORTPL2 cface31e: enum+nullptr, deleted ctor templates, defaulted-arg ties) |
| - | p3/cxx-byvalue-prvalue-arg-moved-not-elided | prvalue C++ class into a by-value param | extra move vs clang elision; TERNELIDE (daa3a99f) covered same-class conditionals; UNARYCALL cface31e covered unary operators over calls |
| - | p3/cpp-reference-member-leftovers-after-t47 | ref members | residue: unwind leak / extra copy / wrapper path, elided-range prvalue/array residue, REFCTX raw-pointer/self-field refusal residue, return-gate untracked spots (frame pointer via local, CFlat callee, lambda capture) after REFESC (af28ab81); opaque-fn alias refusal RULED 2026-10-29 (stays refused), `&f` element RULED (WAI) |
| - | p3/auto-local-of-arithmetic-takes-cflat-width-type-at-cpp-calls | auto local at C++ template call | integer half landed 2026-11-01 (a9e82a96); float and native-call residue |
| - | p3/cpp-wrapper-request-misses-template-argument-headers | `std.unique_ptr<cppon.Cls>` wrappers | WRAPGRP cface31e landed member-type template-argument headers + pointer typedefs; open: noinc implicit dtor, unpublished-specialization pointer binding |
| - | p3/arithmetic-result-type-names-vs-clang | arithmetic result types | RULED 2026-10-31; usual conversions landed (3deaad3e), naming residue |
| - | p3/copy-constructor-overload-resolution-parity + p3/cpp-call-overload-and-copy-count-parity-leftovers | C++ copy ctor / copy-assign pick | const copy assignment picks differently; extra copies at return init |
| - | p3/cpp-free-operator-conversion-vs-builtin-ambiguity + p3/template-member-operators-not-ranked + p3/cpp-operator-ranking-and-fnptr-ctor-leftovers | C++ operator candidate set | imported enum free operators vs built-in; member operator templates not ranked; MIX7 residue (RULED 2026-10-31) |
| - | p3/cpp-null-constant-ctor-leftovers-after-t132 | literal-zero C++ ctor selection | take with NULLCTOR |
| B5b | p3/cpp-constrained-twin-default-argument-ranking | MSVC: same-mangled constrained twins share one emitted body | Itanium landed CONSTTWIN (72073fc6); clang-cl residue only |
| B5g | p3/cpp-callback-pointee-const-ranking-gaps | function-pointer values lose pointee const; clang-refused callback sets accepted | NEW 2026-10-03 (T25 rounds 3-5, pre-existing on master) |
| B5j | p3/cpp-pointer-ref-overload-ctor-and-direct-member-ranking | `const char *const &` overloads: constructor wrapper path and direct non-template ranking; literal at a `const char *&&` template refused | NEW 2026-10-05 (T28 round 5 matrix, pre-existing on master) |
| B14 | p3/c-flexible-array-member-leftovers | raw interior-pointer `move` (RULED 2026-10-31: proven-borrowed raw pointer move refused), `sizeof(*z->data)` 2-D row, siblings | FLEXLEFT cface31e landed `*&` / paren sizeof and implicit-this `move data` |
| B16 | p3/cpp-operator-move-operand-leftovers | template U&&, elision, cond move into forwarding operator=, member-receiver lhs move | NEW 2026-09-28 (D8 report) |
| B18 | p3/cpp-assignment-result-leftovers-after-b9 | by-design B9 rulings (arr elem init landed ASSIGN 72073fc6) | OPEN (P2 landed 1955bd95) |
| L-QUALORDER | p3/qualified-name-root-declared-later-invisible | type order in signatures | namespace members declared later landed QUALORDER (43eceb08); later struct / class / interface / generic types landed QUALORDER2 (af28ab81); open: root globals used before declaration |
| W13b | p3/cpp-overload-variadic-and-conversion-gaps | variadic-set ambiguity, defaulted param before ellipsis, member variadics, user conversions at C++ call args, inline variadic link, non-record const pointers | NEW 2026-09-30 (W13 reviews) |
| G6 | p3/header-cache-residual-growth | in-version prune age rule (sigbase, dead request configs, cxxdemand) | NEW 2026-09-29 (split from G5) |
| B2 | PARKED 2026-09-28 after 3 Codex rounds (scratch/b2_parked.patch): p3/cpp-string-literal-template-deduction-and-unspellable-char-args + p3/cpp-std-min-long-and-pointer-arguments-refused. Restart from master on opus with the narrower design in the issue files | RequestCxxFunctionTemplate argument spelling (CxxStringLiteralSpelling, InferImplicitCxxArgumentType) | One mechanism: spell array / literal lvalues as `*reinterpret_cast<E (*)[N]>(p)` like the member path (e6c60220); give `long` arithmetic, `char*` and pointer-arithmetic rvalues a C++ spelling |
| B3 | p3/cpp-single-level-pointer-unproven-at-call-argument | IsProvenPrimitiveSinglePointerArg / CompareUpconvert | Pointer-is-not-a-number ruling; scheduled as PTRRANK below |

### C - wrongly accepted (adds a rejection)

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| - | p3/cpp-operator-template-set-ambiguity-without-drop-accepted | C++ operator ranking | free-only ambiguous operator template set with no dropped candidate accepted (clang ambiguous); int*/void*/template no-drop case (T85 reviews 3-4, pre-existing) |
| - | p3/constref-byvalue-constructor-overload-ambiguity-accepted + p3/cpp-brace-overload-ambiguity-leftovers-after-t314 | C++ call / brace-arg ranking | CVAMB cface31e landed by-value vs reference class ties (functions, methods, operators, ctors); open: tie through a conversion operator (`fi(u)` via `operator WV()`), brace-argument ties |
| - | p3/postfix-arm-ternary-binds-slot-and-literal-at-char-ptr-ref | ternary arm lvalue residue (TERNADDR) | paren-depth parser overflow, cast-arm address, char-global address join; char* lvalue into const char*& landed 2026-11-04 |

### D - wrongly refused (acceptance gaps)

| Run | Issue(s) | Site | Mode |
|-----|----------|------|------|
| - | p3/cpp-static-local-derived-to-base-and-lvalue-ternary-copies | C++ class ternary + static init leftovers | array element of a temp as an arm, temp dtor exception not propagated, global move init; moved-arm + nested temp field landed 2026-11-04, mixed-class lvalue join landed 2026-11-06 (af28ab81); derived local return implicit move landed STATLEFT cface31e (explicit `return move d` residue) |
| - | p3/function-slot-null-zero-spellings | null-constant classifier | function-like zero macros landed NULLMACFN (cface31e); residue: function-slot ternary join (RULED 2026-10-20) |
| D1b | p3/cpp-unguarded-header-class-template-lookup-fails-cold (cold-cache only) | free-function selection; incremental request units | NEW 2026-09-28 (D1 leftovers) |
| - | p3/cpp-expression-template-to-cflat-byvalue-class-param-refused | converting-ctor path | absorbed by P1 (converting constructors); land there |
| - | p3/cpp-class-pointer-converting-ctor-leftovers | converting-ctor path | T305 residue; with P1 |
| - | p3/cpp-inherited-operator-diamond-and-protected-access | virtual inheritance in imported classes | with P4 virtual-base layout |
| - | p3/cpp-cast-of-lvalue-skips-temporary-copy | assignment to a cast result | not parsed |

### E - temporary lifetime, leak, dangle (one issue per run, opus-tier area)

| Run | Issue | Notes |
|-----|-------|-------|
| E3 | p3/cpp-unwind-cleanup-gaps-ctor-new-temporaries | follow-up list after d59df39b / cpp-unwind2 |
| E4 | p3/cpp-new-remaining-allocator-gaps | follow-up list after cpp-class-operator-new (5 items) |
| E5 | p3/cflat-default-param-temporaries-leftovers + p3/conditional-default-byvalue-cpp-ctor-extra-copy + p3/cpp-default-ternary-arg-leftovers + p3/cpp-member-constref-default-ternary-prvalue | default-argument temporaries (DEFCLS / CDEFARG residue): timing, ternary `default`, extra copy, leak |
| E6 | p3/cflat-move-cpp-class-sink-shapes-leftovers + p3/move-call-temp-subfield-return-argument-miss-member-move + p3/discard-ternary-move-join-and-cxx-field-move | `move` of a C++ class: sink shapes byte-copy, call-result subfield skips member move, discard join |
| E7 | p3/cpp-ternary-move-join-leftovers + p3/cpp-cast-move-temp-arg-and-mixed-ternary-cast-copy + p3/conditional-cpp-lvalue-copy-skipped | ternary / cast move joins over C++ classes |
| E8 | p3/cpp-return-named-local-implicit-move-and-byvalue-elision + p3/nrvo-leftovers-after-t130 + p3/cpp-paren-aggregate-prvalue-member-move | return / NRVO / paren-aggregate elision vs clang |
| E9 | p3/cpp-unbound-wrapper-later-write-overloaded-byvalue | T518 residue |
| E10 | p3/interop-throw-leaks-fresh-movable-temporary | throw during destination destruction |
| E11 | p3/lowered-cxx-field-structs-leftovers-after-t528 | with P2 (cflat-struct-nontrivial-cxx-fields) |

### F - diagnostics only

| Run | Issue(s) | Mode |
|-----|----------|------|
| F1 | p4/localize-relayed-clang-diagnostics + p3/cpp-sink-refusal-cause-lost-under-harvest-suppression | one run: every relayed clang cause surfaces, prefixed `clang: ` (2026-09-27 ruling, no translation). Batch for the prefix sites; the harvest-suppression half is full-mode review |

### G - request cache and incremental infrastructure

| Run | Issue(s) | Mode | Notes |
|-----|----------|------|-------|
| G2 | p3/cpp-wrapper-request-group-omits-type-dependencies + p3/cpp-candidate-tier-differs-cold-vs-warm | batch | RequestGeneratedCxxWrapperUncached, CandidateCxxGroupsFor; scheduled as CACHE-KEY below |
| G3 | p3/cpp-check-mode-cannot-repopulate-type-request-cache | full | design: --check writing the cache |
| G4 | p3/cpp-noinc-mode-divergences | full | CFLAT_CPP_INCREMENTAL=0 kept (RULED 2026-10-31); fallback must bind what incremental binds; two crashes fixed |

### H - coverage only (no known defect)

| Run | Issue | Notes |
|-----|-------|-------|
| H1 | expression-template-ctor half of p3/cpp-inherited-members-and-expr-ctor-coverage-gaps | `requires` / `enable_if` ctor templates, copy/move/dtor counts, by-value `operator=`, same-named records in two namespaces. Cheap |

### N - native ownership, move, borrow (opus-tier area)

| Run | Issue(s) | Notes |
|-----|----------|-------|
| N-MOVESINK | p3/move-sink-leftovers-after-t128 + p3/unique-field-assign-source-not-marked-moved + p3/unique-field-chain-auto-and-array-element-leftovers | ternary / `??` / indirect sinks, unique-field consume gaps (box 9 closed items 14-15) |
| N-FOREACH | p3/unique-foreach-element-adoption-leftovers | capturing-lambda adoption, removeAt shift, read after growth, other containers |
| N-RAWOWN | p3/raw-local-owner-bit-leftovers + p3/owning-pointer-release-withheld-when-alias-may-reach + p3/runtime-null-coalesce-new-into-local-leaks | raw owner bit: counter-dependent first-iteration sink, alias-withheld release (leak only), `g ?? new T` leak |
| N-JOIN | p3/owned-borrowed-ternary-join-rejects-and-misclassifies + p3/moved-raw-heap-array-ternary-destroys-one-element | owned/borrowed `?:` joins; branch fix/t415-heaparrmove parked |
| N-VIEW | p3/borrowed-view-fields-passed-through-retaining-calls + p3/move-borrowed-view-field-refused | view fields through retaining calls; `move` of a borrowed view field refused |
| N-COPY | p3/field-subscript-and-borrowed-param-field-copy-leftovers-after-r22 + p3/pointer-subscript-owning-element-copy-leftovers-after-r21 + p3/raw-delete-string-array-skips-element-dtors | R21 / R22 copy residue; `delete[]` of string* skips element dtors |
| N-BORROW | p3/consolidate-named-variable-borrow-provenance + p3/iface-return-call-borrow-precision + p3/deref-of-moved-pointer-guard-inside-callee | borrow provenance of named variables; interface return precision; false positive behind a terminating callee |
| N-UNIQUE | p3/unique-array-dynamic-index-move-untracked + p3/unique-alias-function-type-parameter-lowering + p3/generic-ifconst-borrowed-global-transfer-skips-dtor | dynamic-index field moves untracked; unique in function types lowered raw; generic if-const transfer skips dtor |
| N-RULED | p3/chained-assign-raw-inner-into-unique-refused + p3/method-on-global-receiver-consumes-field | RULED 2026-10-21, buildable |
| N-TEMP | p3/statement-temp-gep-leaks | two conservative retention cases, no free |
| N-CONST | p3/native-auto-copy-drops-transitive-const + p3/const-gaps-c-depth-auto-copy-and-named-alias-signatures + p3/trailing-const-leftovers-after-c08 | PARKED (branch fix/t348-autoconst); C global exact levels; const plan phase 8 residue |
| - | p3/uninitialized-new-array-reads | ruled 2026-09-03; no detection today |
| - | p3/null-safe-new-temp-chain-leftovers | pointer results leak by design; record only |

### L - native language surface

| Run | Issue(s) | Notes |
|-----|----------|-------|
| L-TUPLE | p3/tuple-leftovers-after-tuplit | ruled 2026-11-01; TUPALIAS 43eceb08 + TUPLEFT 43eceb08 landed; residue: new S((nullptr,4)), owned unique-element refusals, R133/R134 |
| L-GENERIC | p3/generic-instantiation-value-leftovers + p3/generic-literal-deduction-leftovers | overload sets landed 72073fc6; C++ template instance as value; ctor deduction (ruled 2026-10-13) |
| L-PERF | p3/nested-ternary-compile-time | 2x per `?:` level fixed by b9f39cdb; residue: early-pass walkers overflow on deep paren / else-if / `((x)+1)` nesting |

### P - plans and staged work

| Id | Item | Notes |
|----|------|-------|
| P1 | plan converting-constructors (`internal/plan/converting-constructors.md`) | ruled 2026-09-26; eigen_05; absorbs D-row converting-ctor rows. Largest item, own staging |
| P2 | plan cflat-struct-nontrivial-cxx-fields (`internal/plan/cflat-struct-nontrivial-cxx-fields.md`) | ruled A 2026-09-27. Phases 1-4 one per branch. Absorbs p3/cpp-typed-local-extra-copy-from-param-and-ctor-arms, E11 |
| P3 | p3/cpp-std-header-survey-refresh | re-run the std header L0/L1 survey on macOS and Windows to refresh the gap list; untouched headers need a CFlat-surface ruling |
| P4 | virtual-base layout (item 2 of p3/cpp-inherited-member-access-and-virtual-base-diagnostics) + p3/cpp-new-virtual-base-class-unsized | refused today ("layout cannot be reproduced"; `new VB()` RULED 2026-10-25 by design until the layout lands); needs a plan |
| P5 | p4/owning-nullable-string | RULED 2026-10-21: finish `T?` as a planned feature, plan `internal/plan/nullable-types.md` |
| P6 | p3/no-incremental-build-and-no-up-to-date-check | RULED 2026-08-22; whole-program builds |
| P7 | plan cpp-one-allocator-family (`internal/plan/cpp-one-allocator-family.md`) | RULED 2026-11-04: one new/delete family so C++ allocates from a `program` thread's allocator; two families until it lands |

### Platform, runtime library, tooling

| Issue | Notes |
|-------|-------|
| p3/cross-target-compile-gaps | cross-target (-p win64) leftovers |
| p3/os-abstraction-process-cb-not-converted | cheap singleton |
| p3/no-tls-in-core-network | TLS unsupported on Linux |
| p3/thread-cannot-go-raii | two independent blockers |
| p3/pools-no-destructor-shutdown-ordering | manual by design; record only |
| p3/generated-code-is-roughly-twice-the-size-msvc-emits | codegen size vs MSVC |
| p3/lsp-slow-after-aborted-interop-compile | LSP ~491 s after an aborted interop compile |
| p3/test-move-with-cpp-import-self-return-proof-refused | test_move with a C++ import reaches a later unique reset refusal |
| p3/edge-fill-winui + p3/listview-dark-column-rules | Windows UI; Windows box only |
| p3/clang-tidy-performance-backlog | ~232 string building left; profile-gated or turn the check off (other perf checks landed 2026-11-07) |

### New or rewritten 2026-09-29 (leftovers from landed rows)

| Run | Issue | Summary | Status |
|---|---|---|---|
| N3 | p3/cpp-const-object-nonconst-member-call-writes-readonly | loop back-edge const-pointer dataflow, virtual-base twin | W13 landed const auto copy, const globals, const-pointer overload selection 2026-09-30; rest open |
| N10 | p3/cpp-brace-argument-backing-remaining-shapes | Brace-list backing: optional constructor parameter case remains | NEW 2026-09-29 |
| N12 | p3/cpp-conversion-operator-receiver-leftovers | C++ conversion-operator leftovers: template operator U() only | CONVOP 72073fc6 landed the rest |
| N13 | p3/cpp-ctor-expression-position-const-ref-copies-lvalue | C++ ctor in expression position - remaining `const T&` copies and enum `T&&` refusal | NEW 2026-09-29 |
| N15 | p3/cpp-default-wrapper-batch-chunk-stores-stripped-prefix | C++ header default-wrapper batch chunk stores a stripped prefix (latent replay failure) | scheduled as CHUNK |
| N24 | p3/cpp-unrequested-specialization-pointer-eager-request | C++ interop compile time; left by D6 | perf |
| N28 | p3/cpp-deferred-special-members-mid-size-harvest-cost | R1 deferral costs +1-2% cold on 64..~500-record harvests (interop fixtures); cutoff adjustable later (maintainer) | perf |
| N31 | p3/cpp-interpreter-drops-cc1-only-flags | -Xclang cc1-only flags silently dropped by the Interpreter driver (pre-existing, H1 review) | perf |
| N34 | p3/cpp-deployment-target-env-core-cache-miss | Non-default MACOSX_DEPLOYMENT_TARGET misses the core bitcode cache every compile (R4 review) | perf |
| N35 | p3/cpp-demand-bodies-torch-cold-cost | R4 costs +4.4% cold instructions on torch (json -28%); verdict writes batched in 93b361f6 (-0.55%), rest is clang instantiation | PART 2026-09-29d |
| N37 | p3/cpp-signature-registration-projects-records | Registration projects every record a signature names; -3.6% torch only by skipping projection, which breaks overload order (D4) | perf |
| N41 | p3/cpp-variadic-alias-template-refused | pack patterns landed 43eceb08 (VARALIAS); residue: non-type EXPRESSION alias arguments (pack and non-pack) refused | NEW 2026-10-01 |
| N42 | p3/cpp-header-unreferenced-global-initializers-skipped | residue: root-const / qualified-const `if const` order, non-inline extern destructible shared global, same guard macro in two files | p3 |
| N47 | p3/cpp-constant-fold-and-const-static-twin-leftovers | constexpr-call initializer refused by the fold guard; const receiver + static twin overload refused (ST4 re-review leftovers) | NEW 2026-10-01 |
| N52 | p3/cpp-range-for-member-name-lookup-and-count-probe | range-for member lookup misses enum/nested-type begin/end (accepts invalid); count/get clients pay a failed ADL probe; ST6 follow-up | NEW 2026-10-01 |
| N58 | p3/cpp-struct-identity-underscore-namespace | [cpp] identity: ns_ namespace decode, global name collision, CRTP base reading a derived field; scheduled as NSLOOKUP | NEW 2026-10-01 |

### std_full tier-2 gaps (test_libs/std_full DISABLED cases; plan internal/plan/std-library-tier2-suite.md)

`DISABLED_WIN` / `DISABLED_MAC` disable a case on one platform only (MSVC-only / libc++-only gaps). Fixing a row removes the DISABLED marker of its case(s) in the same commit (`./test_libs.sh -t 2 std_full --include-disabled` shows XPASS).

| Issue | DISABLED case | Summary |
|---|---|---|
| p3/cpp-reference-param-conversion-gaps-after-t42 | (C++ interop) | template ctor from non-template source, inherited operator X(), operator X&/X&& into C++ ref params refused (clang accepts; T42 follow-up) |
| p3/cpp-pointer-depth-and-array-pointer-param-gaps | (C++ interop) | non-null int** accepted into C++ int***; member/static array-pointer params refuse valid args |
| p3/generic-overload-not-joining-concrete-overload-set | (none) | refused cells landed GENOVL (ef83ffa02); ruled picks landed GENOVLR (5344802ed); leftovers: CFlat expression typing (1 << 2L, 1.5 + x, char arithmetic), strlit vs char*, named-arg inference, generic-only const pointee return, UFCS free generic |
| p3/member-type-path-remaining-gaps | (none) | `Outer<int>.Inner<long>.type`, closure-alias members, CFlat static data members, C++ statics as value template args (T37 follow-up) |

### Ruled - ready to schedule (rulings recorded in each issue file)

| Issue | Work |
|---|---|
| p3/alias-return-byvalue-param-leftovers-after-b21 | item 5 ratified (extern C callee owns by-value params) - no code |
| p4/three-way-comparison-operator | `<=>` in the CFlat language RULED 2026-10-02, plus defaulted member-wise `==` |
| p3/new-pointer-depth-carrier | RULED 2026-11-04: lift the depth-2 cap; step 1 (spelled depth <= 31) and step 2a (`new`, generic substitution, const deduction; 43eceb08) landed, step 2b (C/C++ interop depth > 2) open |

### Grouped runs 2026-10-10 (ranked; source scratch/issue_groups_2026-10-10.md; landed rows removed)

Score = value / (complexity x risk), value 1.5 per p2 + 1 per p3; C = S/M/L, R = L/M/H (H = C++ overload / CInterop
wrappers / request cache / ownership, or torch). Cheap = fits a Haiku implementer (anchored, risk L-M). Sites marked (u)
come from issue text only - confirm by grep before the run. The sections above stay the per-issue record; schedule from
this table.

| # | Run | C/R | Issues | Shared site / root cause | Not concurrent with | Cheap |
|---|-----|-----|--------|--------------------------|---------------------|-------|
| 1 | DIAG-TEXT | M/L | p3/cpp-sink-refusal-cause-lost-under-harvest-suppression | shared KIND only (relayed clang / refusal text) (u) | F1 | yes |
| 3 | NULLCTOR | L/M | p3/cpp-null-constant-ctor-leftovers-after-t132 | scalar-ref + nullptr_t&& + tdp<int>(0) landed 72073fc6; function-like zero macros landed cface31e | CTOR-VIABILITY, PTRRANK | no |
| 4 | NSLOOKUP | M/M | p3/cpp-struct-identity-underscore-namespace | CRTP field item landed 72073fc6; residue: [cpp] struct vs same-named imported global class (identity table, direction coexist) | - | yes, careful on identity |
| 8 | PTRRANK | M/H | p3/cpp-single-level-pointer-unproven-at-call-argument, p3/cpp-pointer-depth-and-array-pointer-param-gaps | CompareUpconvert (Overloads), IsProvenPrimitiveSinglePointerArg, TryBinaryOperatorOverload | NULLCTOR, CTOR-VIABILITY | no |
| 10 | CLIBNAME | M/M | p3/core-defines-externally-visible-libc-functions | core defines libc names that LLVM folds as builtins | - | RULED 2026-10-21: nobuiltin on CFlat bodies |
| 14 | CONVOP | M/H | p3/cpp-conversion-operator-receiver-leftovers | landed 72073fc6 except template `operator U()` (incl. T122 case) | - | no |
| 15 | ASSIGN | L/M | p3/cpp-assignment-result-leftovers-after-b9 | route + c6 landed 72073fc6 (route-leftovers deleted); residue: by-design B9 rulings only | - | no |
| 16 | REFPARAM | M/H | p3/cpp-reference-param-conversion-gaps-after-t42, p3/cpp-reference-member-leftovers-after-t47 | conversions into C++ reference params (u) | CTOR-VIABILITY | no |
| 18 | CTOR-VIABILITY | L/H | p3/cpp-ctor-template-overload-and-copyinit-gaps, p3/cpp-pointer-ref-overload-ctor-and-direct-member-ranking | CxxCtorSetRequiresNarrowing + ctor wrappers | NULLCTOR, REFPARAM, PTRRANK | no |
| 20 | FNWRAP-RES | L/H | p3/cpp-forwarded-closure-refusals-after-t525, p3/cpp-capturing-closure-to-std-function | residue after FNWRAP af28ab81 (see issue files) | ELIDE (P2 plan) | no |
| 22 | CHUNK | M/H | p3/cpp-demand-chunk-order-numbering-per-interpreter, p3/cpp-default-wrapper-batch-chunk-stores-stripped-prefix | chunk store, CxxIncrementalGroup ParseRequest | CACHE-KEY, A3 | no |
| 25 | CONSTTWIN | L/H | p3/cpp-constrained-twin-default-argument-ranking | Itanium + const twins landed 72073fc6; residue: MSVC shared-symbol body choice (Windows host) | CONSTPOINTEE | no |
| 26 | SRCTYPE | L/H | p3/auto-local-of-arithmetic-takes-cflat-width-type-at-cpp-calls | integer + enum half landed 72073fc6; residue: floating auto local (float/double) at C++ calls | - | no |
| 27 | CACHE-KEY | L/H | p3/cpp-wrapper-request-group-omits-type-dependencies, p3/cpp-wrapper-request-misses-template-argument-headers, p3/cpp-candidate-tier-differs-cold-vs-warm | RequestGeneratedCxxWrapperUncached, CandidateCxxGroupsFor; partial patch scratch/repro_keep/t70_groupfix.patch | CHUNK, G3 | no |
| 28 | BORROW-PROV | L/H | p3/consolidate-named-variable-borrow-provenance | borrow provenance of named variables through helpers; design first (N-BORROW) | N runs | no |

Pairs: CONSTPOINTEE = p3/cpp-callback-pointee-const-ranking-gaps (typed-null half closed by C05B).
Cheap singletons (S, risk L): p3/os-abstraction-process-cb-not-converted, p3/view-element-gate-diagnostic-gaps.
Overload/ctor selection family (never two at once): NULLCTOR, PTRRANK, CTOR-VIABILITY, CONSTTWIN, CONSTPOINTEE.
Stale references found by the regroup: the std_full table named a static-auto prvalue / copy-initializer storage issue that was never filed (no file in git history, checked 2026-11-06);

### Parked - needs a maintainer ruling before any work

| Issue | Question |
|-------|----------|
| (none) | all ruled 2026-11-04 |

### Conflict map (never concurrent)

The overload/ctor family (NULLCTOR, PTRRANK, CTOR-VIABILITY, CONSTTWIN, CONSTPOINTEE, REFPARAM) all touch
LLVMBackend_Overloads ctor/argument selection - run them one after another. CONVOP and the operator ranking in MainListener_BinaryOperators share operator dispatch.
A3 and CHUNK share the incremental action. CACHE-KEY -> G3 share the request-cache key. N runs share the ownership
dataflow in MainListener / nulldf - one native ownership run at a time.
