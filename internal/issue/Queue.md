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

Timing note (2026-11-14): a build-stamp flip over a full local cache now costs ~1% (test.sh 545 s vs 542 s
cleared); clearing before a timed run is no longer needed.

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

p1 = 0. p2 = 3 open, one (LINT) blocks on a ruling:

- p2/expect-error-substring-source-check-loose - NEEDS RULING (2026-11-18 box 16: literal anchoring refuses 386 legit substrings; two options in the file).
- p2/cold-cpp-interop-compile-peaks-23-27-gb - 9.51 GB cold since the regroup (80a853e9a; was 20.33 GB); accepted until plan P10 lands (RULED 2026-11-18).
- p2/test-suite-over-time-budget - run G7: phase timing first; a fixture split needs the maintainer's OK.

Run the Windows gate on the master tip once per box.

## In flight

None. No parked branch or worktree exists (the old parked branches were deleted 2026-11-18; only `fix/movedtor` is left).

## Ready to start (ruled, nothing open - schedule from here first)

Each ruling is in the issue file, which is the brief's source. One run = one worktree. Pick top down; rows that share a site are marked.

| Run | Issue | Mode | Work | Not concurrent with |
|-----|-------|------|------|---------------------|
| CLIBNAME | p3/core-defines-externally-visible-libc-functions | needs ruling | the RULED part (nobuiltin) landed as T444; what remains (a C object / C++ inline calling printf resolves to the core body) is outside that ruling | - |
| - | p4/three-way-comparison-operator | plan-sized | `<=>` in the CFlat language RULED 2026-10-02; `= default` member-wise `<=>` still needs a ruling | operator runs |
| P10 phase 1 | plan cpp-import-module-reuse | read-only spike | DONE 2026-11-18: only a modular std works (ruled out); back to the maintainer, see the plan | - |

Ruled but NOT ready: P9 `static` fields (two questions before phase 1, in the plan), P8 runtime-sized arrays (zero bound and owning elements not ruled; optional).

## Residue open (rest of the issue landed)

| Issue | Open |
|-------|------|
| p3/moved-from-struct-leftovers-after-t490 | callee destroys by-value params always (RULED 2026-10-29, CFlat ABI; T499 attempt backed out), conditional consumes need the callee drop flag |
| p3/interface-call-overload-ranking-and-conversion | native ConstMask hole (needs core const annotations), first-not-ambiguous picks, interface single-slot widen, native pointer-field const carry, const twin mangling (all pre-existing) |
| p3/ctor-lambda-this-closure-relocation | by-value return of a this-capturing-closure holder awaits the native return-ABI ruling |
| p3/self-pointer-struct-return-not-in-place | RULED 2026-10-25: C++ guaranteed elision for every ctor-/dtor-bearing native struct (sret + in-place new); plan first |
| p3/cpp-mixed-class-conditional-value-return-argument-refused | S3 sret copy, lv_bv, nested mixed, CFlat/C++ temp order, win64 cross-compile refusal |
| p3/alias-return-byvalue-param-leftovers-after-b21 | items 1, 2, 4, 6 open; item 3 on hold (2026-10-15); item 5 ratified, no code |

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
| - | p3/cpp-using-declaration-member-access-gaps | virtual base re-exposed by `using` | with P4 virtual-base layout |
| - | p3/cpp-macro-constant-leftovers-after-t98 | macro identity + brace fold | residue: `auto&&`, anonymous-enum lvalue deduction / joins, comma lvalue category |
| - | p3/cxx-byvalue-prvalue-arg-moved-not-elided | prvalue C++ class into a by-value param | conditional / recursive / mk2(mk(1)) callees closed by RETELIDE cc2d19b64; residue: a later converting argument beside a deleted / private-move prvalue (refused, clang accepts) |
| - | p3/cpp-reference-member-leftovers-after-t47 | ref members | residue: brace-form ctor args / extra copy / non-inert-arg wrapper path, elided-range prvalue/array, REFCTX raw-pointer/self-field refusal, return-gate untracked spots (frame pointer via local, CFlat callee, lambda capture); opaque-fn alias refusal and `&f` element RULED 2026-10-29 (stay as is) |
| - | p3/auto-local-of-arithmetic-takes-cflat-width-type-at-cpp-calls | auto local at C++ template call | float and native-call residue |
| - | p3/cpp-wrapper-request-misses-template-argument-headers | `std.unique_ptr<cppon.Cls>` wrappers | open: noinc implicit dtor, unpublished-specialization pointer binding |
| - | p3/copy-constructor-overload-resolution-parity + p3/cpp-call-overload-and-copy-count-parity-leftovers | C++ copy ctor / copy-assign pick | template-ctor return pick + CFlat-callee elision landed cc2d19b64 (RETELIDE); residue: lambda-capturing callee keeps the extra copy (NEEDS RULING: drop the gate + re-pin bridge_capture_return to copy 1 / dtor 2), CALLPAR landed 069983274 (competing `?:` default, bare default ambiguity, ADL, template-only deferral); leftovers file = remaining pre-existing parity shapes |
| - | p3/cpp-class-assignment-false-refusals | C++ same-class assignment | NEW 2026-11-19 from CPPOPEQ e3eb8921f (deleted / inaccessible / ambiguous operator= now refused like clang): C++ call-result source and target landed 14c9bd018 (ASGACC: register-returned sources, `mk() = b`); residue: [cpp] struct assignment, MoveOnly `?:` text; reference-member class still accepted NEEDS RULING |
| - | p3/cpp-operator-ranking-and-fnptr-ctor-leftovers | C++ operator candidate set | imported enum free operators vs built-in (member templates vs free operators landed d6eeafe54) |
| - | p3/cpp-null-constant-ctor-leftovers-after-t132 | literal-zero C++ ctor selection | only item 3 (char-only ctor narrowing, by design unless ruled) left |
| B5b | p3/cpp-constrained-twin-default-argument-ranking | MSVC: same-mangled constrained twins share one emitted body | clang-cl residue only |
| B14 | p3/c-flexible-array-member-leftovers | raw interior-pointer `move` (RULED 2026-10-31: proven-borrowed raw pointer move refused), siblings | residue in the issue file |
| B18 | p3/cpp-assignment-result-leftovers-after-b9 | by-design B9 rulings | residue: by-design B9 rulings only |
| G6 | p3/header-cache-residual-growth | in-version prune age rule (sigbase, dead request configs, cxxdemand) | NEW 2026-09-29 (split from G5) |
| G7 | p2/test-suite-over-time-budget | test.sh phases / test_cpp_interop critical path | NEW 2026-11-17: idle warm 357 s, cold 675 s vs ~200/~300 budget; phase timing first, fixture split needs OK |

### C - wrongly accepted (adds a rejection)

| Run | Issue(s) | Site | Notes |
|-----|----------|------|-------|
| - | p3/cpp-brace-overload-ambiguity-leftovers-after-t314 | C++ call / brace-arg ranking | open: template candidate with a braced list, a tie across two conversion operators by value, cosmetic candidate-list spelling |

### D - wrongly refused (acceptance gaps)

| Run | Issue(s) | Site | Mode |
|-----|----------|------|------|
| - | p3/cpp-static-local-derived-to-base-and-lvalue-ternary-copies | C++ class ternary + static init leftovers | residue: global move init; explicit `return move d` |
| - | p3/function-slot-null-zero-spellings | grammar | OPEN residue: `rv.G{0}` expression form does not parse (data-pointer null casts landed 63cdc54c2) |
| - | p3/cpp-expression-template-to-cflat-byvalue-class-param-refused | converting-ctor path | absorbed by P1 (converting constructors); land there |
| - | p3/cpp-class-pointer-converting-ctor-leftovers | converting-ctor path | T305 residue; with P1 |
| - | p3/cpp-inherited-operator-diamond-and-protected-access | virtual inheritance in imported classes | with P4 virtual-base layout |

### E - temporary lifetime, leak, dangle (one issue per run, opus-tier area)

| Run | Issue | Notes |
|-----|-------|-------|
| E3 | p3/cpp-unwind-cleanup-gaps-ctor-new-temporaries | item 2 landed a2111ce54; item 3 Windows funclet cleanup not exercised |
| E4 | p3/cpp-new-remaining-allocator-gaps | by-design / out-of-scope residue only (bare `Sx(...)` in a reopened namespace beside a global function), Windows MS-ABI array new / delete names; volatile copy closed by ruling 2026-11-18 |
| E5 | p3/cflat-default-param-temporaries-leftovers | residue after DEFTMP 9d70b56d0 (call-site native defaults die once): forwarding-wrapper defaults (earlier-param, caller above callee, unique/closure/fn-ptr/C++ holder) leak under a borrow-returning wrapper |
| E6 | p3/discard-ternary-move-join-and-cxx-field-move | C++ class field `_ = move p->c` landed 3c95a46e2 (TERNREST); native string / dtor-struct join leaks landed 46fb989b8 (STRJOIN); residue: raw owning field discard, converting-ctor gap; bare all-move join statement keeps both sources NEEDS RULING (refuse like bare `move o.d;` or discard) |
| E7 | p3/cpp-ternary-move-join-leftovers | TERNLEFT 0b6b7e59a + TERNREST 3c95a46e2 closed discarded / nested / place-arm joins and [cpp] mixed decls; residue: argument order, const leaf in nested join, [cpp] mixed return, no-move discard of place + prvalue, const by-value params |
| E8 | p3/nrvo-leftovers-after-t130 + p3/cpp-paren-aggregate-prvalue-member-move | return / NRVO / paren-aggregate elision vs clang; brace-element in-place build landed 5bda3c58a (BRACEELIDE: unreachable local reads, call/temp args, char* const* params, field/array call slots); residue: see "Still open" in the nrvo file |
| E9 | p3/cpp-unbound-wrapper-later-write-overloaded-byvalue | residue: same-arity overload sets, later C++ default param, mixed template arity (bound-wrapper divergences recorded) |
| E11 | p3/lowered-cxx-field-structs-leftovers-after-t528 | with P2 (cflat-struct-nontrivial-cxx-fields) |
| E12 | p3/cpp-override-callee-destroyed-byvalue-param-double-move | trivial_abi param through C++ dispatch moves twice (needs a per-function caller-destroyed convention); MS ABI: OVRMOVE 82c23505a is a no-op (inferred) |

### F - diagnostics only

| Run | Issue(s) | Mode |
|-----|----------|------|
| F1 | p4/localize-relayed-clang-diagnostics + p3/cpp-sink-refusal-cause-lost-under-harvest-suppression | `linker: ` prefix landed 88e7b6def (LINKPFX; clang driver line keeps `clang: `); open: audit of unprefixed clang sinks (batch), translation question, harvest-suppression half (full-mode review) |

### G - request cache and incremental infrastructure

| Run | Issue(s) | Mode | Notes |
|-----|----------|------|-------|
| G3 | p3/cpp-check-mode-cannot-repopulate-type-request-cache | full | design: --check writing the cache |
| G4 | p3/cpp-noinc-mode-divergences | full | calloc / operator= / esfts base landed 5f56a7576; residue: noinc first stops interop 282 (eager broken inline bodies), template 431 (CRTP over incomplete [cpp] struct), bridge 3504 (__cflat_user undeclared) |

### N - native ownership, move, borrow (opus-tier area)

| Run | Issue(s) | Notes |
|-----|----------|-------|
| N-MOVESINK | p3/unique-field-assign-source-not-marked-moved | indirect-root consumes landed 0050627c8 (MOVESINK; move-sink-leftovers file closed); residue: sink-path false refusals through pointer roots, global/static implicit consume error, conservative misses, operator-call re-init, do-while condition |
| N-FOREACH | p3/unique-foreach-element-adoption-leftovers | items 1, 4 + pointer-collection / global-pointer ranges landed 43f829835 (FOREACHADOPT); item 5 (C++ begin()/end() containers) NEEDS RULING; item 6 `sink2(c ? p : nullptr, p->id)` crash = shared conditional-transfer ordering |
| N-RAWOWN | p3/raw-local-owner-bit-leftovers + p3/owning-pointer-release-withheld-when-alias-may-reach + p3/runtime-null-coalesce-new-into-local-leaks | raw owner bit: counter-dependent first-iteration sink, alias-withheld release (leak only), `g ?? new T` leak |
| N-JOIN | p3/owned-borrowed-ternary-join-rejects-and-misclassifies + p3/moved-raw-heap-array-ternary-destroys-one-element | imported-class prvalue arm beside an unmoved non-const place landed 3080d7a0c (OBJOIN); residue: const place arm (run CONSTJOIN in flight); moved raw heap array ternary |
| N-VIEW | p3/borrowed-view-fields-passed-through-retaining-calls + p3/move-borrowed-view-field-refused | view fields through retaining calls; `move` of a borrowed view field refused |
| N-COPY | p3/raw-delete-string-array-skips-element-dtors | residue after 56aecbae4 (pointer-subscript reads copy, stores drop old): `delete[_]` element dtors NEEDS RULING (LANGUAGE.md + test_cpp_interop:20498 say frees only; options in the file) |
| N-BORROW | p3/consolidate-named-variable-borrow-provenance + p3/iface-return-call-borrow-precision + p3/deref-of-moved-pointer-guard-inside-callee | borrow provenance of named variables; interface return precision; false positive behind a terminating callee |
| N-UNIQUE | p3/unique-array-dynamic-index-move-untracked | dynamic-index field moves untracked |
| N-RULED | p3/chained-assign-raw-inner-into-unique-refused | parameter half landed 2026-11-18 (a pointer parameter adopts like a local); NEEDS RULING: may a raw field or global own implicitly, or must an owning field be spelled unique (spike numbers in the file) |
| N-TEMP | p3/statement-temp-gep-leaks | two conservative retention cases, no free |
| N-CONST | p3/native-auto-copy-drops-transitive-const + p3/const-gaps-c-depth-auto-copy-and-named-alias-signatures + p3/trailing-const-leftovers-after-c08 | no branch (deleted 2026-11-18, redo from the issue files); C global exact levels; const plan phase 8 residue |
| - | p3/uninitialized-new-array-reads | ruled 2026-09-03; no detection today |
| - | p3/null-safe-new-temp-chain-leftovers | pointer results leak by design; record only |

### L - native language surface

| Run | Issue(s) | Notes |
|-----|----------|-------|
| L-TUPLE | p3/tuple-leftovers-after-tuplit | ruled 2026-11-01; R133/R134 closed by UNIQARG b04ec5235; residue: owned unique-element refusals, brace-init field borrows |
| L-GENERIC | p3/generic-instantiation-value-leftovers + p3/generic-literal-deduction-leftovers | C++ template instance as value; ctor deduction `Box(3)` landed 8d05c9344; brace `Box b{3}` NEEDS RULING (aggregate deduction vs designated brace init) |

### P - plans and staged work

| Id | Item | Notes |
|----|------|-------|
| P1 | plan converting-constructors (`internal/plan/converting-constructors.md`) | ruled 2026-09-26; eigen_05; absorbs D-row converting-ctor rows. Largest item, own staging |
| P2 | plan cflat-struct-nontrivial-cxx-fields (`internal/plan/cflat-struct-nontrivial-cxx-fields.md`) | ruled A 2026-09-27. Phases 1-4 one per branch. Absorbs E11 |
| P3 | p3/cpp-std-header-survey-refresh | re-run the std header L0/L1 survey on macOS and Windows to refresh the gap list; untouched headers need a CFlat-surface ruling |
| P4 | virtual-base layout (item 2 of p3/cpp-inherited-member-access-and-virtual-base-diagnostics) + p3/cpp-new-virtual-base-class-unsized | refused today ("layout cannot be reproduced"; `new VB()` RULED 2026-10-25 by design until the layout lands); needs a plan |
| P5 | p4/owning-nullable-string | RULED 2026-10-21: finish `T?` as a planned feature, plan `internal/plan/nullable-types.md`; RULED 2026-11-18: `T?` stays a built-in nullable pointer, `string?` a borrow, optional values deferred |
| P6 | p3/no-incremental-build-and-no-up-to-date-check | RULED 2026-08-22; whole-program builds |
| P7 | plan cpp-one-allocator-family (`internal/plan/cpp-one-allocator-family.md`) | RULED 2026-11-04: one new/delete family so C++ allocates from a `program` thread's allocator; two families until it lands |
| P8 | p4/runtime-sized-arrays-vla | RULED 2026-11-18: `T[n] a` with a runtime bound = C VLA, optional feature; zero bound and owning elements not ruled; refused until built (run ARRBOUND) |
| P9 | plan cflat-static-fields (`internal/plan/cflat-static-fields.md`) | RULED 2026-11-18: `static` struct field = shared storage (C++ / C# meaning); 4 phases, not started; issue p3/member-type-path-remaining-gaps section 1 |
| P10 | plan cpp-import-module-reuse (`internal/plan/cpp-import-module-reuse.md`) | RULED 2026-11-08 + 2026-11-18: dependency reached through its own module, no candidate skip; phase 1 design spike; issue p2/cold-cpp-interop-compile-peaks-23-27-gb |

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
| p3/edge-fill-winui + p3/listview-dark-column-rules | Windows UI; Windows box only |
| p3/clang-tidy-performance-backlog | ~232 string building left; profile-gated or turn the check off |

### New or rewritten 2026-09-29 (leftovers from landed rows)

| Run | Issue | Summary | Status |
|---|---|---|---|
| N3 | p3/cpp-const-object-nonconst-member-call-writes-readonly | loop back-edge const-pointer dataflow, virtual-base twin | open |
| N10 | p3/cpp-brace-argument-backing-remaining-shapes | Brace-list backing: reference packs, multi-parameter and non-type pack patterns, class-prvalue element copies | NEW 2026-09-29 |
| N15 | p3/cpp-default-wrapper-batch-chunk-stores-stripped-prefix | C++ header default-wrapper batch chunk stores a stripped prefix (latent replay failure) | scheduled as CHUNK |
| N24 | p3/cpp-unrequested-specialization-pointer-eager-request | C++ interop compile time; left by D6 | perf |
| N28 | p3/cpp-deferred-special-members-mid-size-harvest-cost | R1 deferral costs +1-2% cold on 64..~500-record harvests (interop fixtures); cutoff adjustable later (maintainer) | perf |
| N31 | p3/cpp-interpreter-drops-cc1-only-flags | -Xclang cc1-only flags silently dropped by the Interpreter driver (pre-existing, H1 review) | perf |
| N34 | p3/cpp-deployment-target-env-core-cache-miss | Non-default MACOSX_DEPLOYMENT_TARGET misses the core bitcode cache every compile (R4 review) | perf |
| N35 | p3/cpp-demand-bodies-torch-cold-cost | R4 costs +4.4% cold instructions on torch (json -28%); verdict writes batched in 93b361f6 (-0.55%), rest is clang instantiation | PART 2026-09-29d |
| N37 | p3/cpp-signature-registration-projects-records | Registration projects every record a signature names; -3.6% torch only by skipping projection, which breaks overload order (D4) | perf |
| N42 | p3/cpp-header-unreferenced-global-initializers-skipped | residue: root-const / qualified-const `if const` order, non-inline extern destructible shared global, same guard macro in two files | p3 |
| N58 | p3/cpp-struct-identity-underscore-namespace | [cpp] identity: ns_ namespace decode, global name collision, CRTP base reading a derived field; scheduled as NSLOOKUP | NEW 2026-10-01 |

### std_full tier-2 gaps (test_libs/std_full DISABLED cases; plan internal/plan/std-library-tier2-suite.md)

`DISABLED_WIN` / `DISABLED_MAC` disable a case on one platform only (MSVC-only / libc++-only gaps). Fixing a row removes the DISABLED marker of its case(s) in the same commit (`./test_libs.sh -t 2 std_full --include-disabled` shows XPASS).

| Issue | DISABLED case | Summary |
|---|---|---|
| p3/generic-overload-not-joining-concrete-overload-set | (none) | `f(const int*)` + `f<T>(T*)` concrete pick CONFIRMED 2026-11-18; residue in the issue file |
| p3/member-type-path-remaining-gaps | (none) | item 1 CFlat `static` field -> plan P9; item 2 open: `if const` C++ static above int32 |

### Grouped runs 2026-10-10 (ranked; landed rows removed; ready runs are listed under Ready to start)

Score = value / (complexity x risk), value 1.5 per p2 + 1 per p3; C = S/M/L, R = L/M/H (H = C++ overload / CInterop
wrappers / request cache / ownership, or torch). Cheap = fits a Haiku implementer (anchored, risk L-M). Sites marked (u)
come from issue text only - confirm by grep before the run. The sections above stay the per-issue record; after Ready to start, schedule from
this table.

| # | Run | C/R | Issues | Shared site / root cause | Not concurrent with | Cheap |
|---|-----|-----|--------|--------------------------|---------------------|-------|
| 1 | DIAG-TEXT | M/L | p3/cpp-sink-refusal-cause-lost-under-harvest-suppression | shared KIND only (relayed clang / refusal text) (u) | F1 | yes |
| 3 | NULLCTOR | L/M | p3/cpp-null-constant-ctor-leftovers-after-t132 | residue: item 3 only (char-only ctor narrowing) | CTOR-VIABILITY, PTRRANK | no |
| 4 | NSLOOKUP | M/M | p3/cpp-struct-identity-underscore-namespace | residue: [cpp] struct vs same-named imported global class (identity table, direction coexist) | - | yes, careful on identity |
| 15 | ASSIGN | L/M | p3/cpp-assignment-result-leftovers-after-b9 | residue: by-design B9 rulings only | - | no |
| 16 | REFPARAM | M/H | p3/cpp-reference-member-leftovers-after-t47 | conversion-gaps file closed by REFCONV 26c853ae7 (`operator X&&()`, inherited ref operators, nonzero-offset base); residue: reference members | CTOR-VIABILITY | no |
| 20 | FNWRAP-RES | L/H | p3/cpp-forwarded-closure-refusals-after-t525, p3/cpp-capturing-closure-to-std-function | residue after FNWRAP (see issue files) | ELIDE (P2 plan) | no |
| 22 | CHUNK | M/H | p3/cpp-demand-chunk-order-numbering-per-interpreter, p3/cpp-default-wrapper-batch-chunk-stores-stripped-prefix | chunk store, CxxIncrementalGroup ParseRequest | CACHE-KEY, A3 | no |
| 25 | CONSTTWIN | L/H | p3/cpp-constrained-twin-default-argument-ranking | residue: MSVC shared-symbol body choice (Windows host) | CONSTPOINTEE | no |
| 26 | SRCTYPE | L/H | p3/auto-local-of-arithmetic-takes-cflat-width-type-at-cpp-calls | residue: floating auto local (float/double) at C++ calls | - | no |
| 27 | CACHE-KEY | L/H | p3/cpp-wrapper-request-group-omits-type-dependencies, p3/cpp-wrapper-request-misses-template-argument-headers | residue after 2df193301 (key + cold/warm tiers fixed): wrapper-body refusal guard; CFLAT_CPP_INCREMENTAL=0 calloc overload | CHUNK, G3 | no |
| 28 | BORROW-PROV | L/H | p3/consolidate-named-variable-borrow-provenance | borrow provenance of named variables through helpers; design first (N-BORROW) | N runs | no |

Cheap singletons (S, risk L): p3/os-abstraction-process-cb-not-converted, p3/view-element-gate-diagnostic-gaps.
Overload/ctor selection family (never two at once): NULLCTOR, PTRRANK, CTOR-VIABILITY, CONSTTWIN.

### Parked - needs a maintainer ruling before any work

| Issue | Question |
|-------|----------|
| p3/chained-assign-raw-inner-into-unique-refused | item 2, raw field / global slots only (parameters landed 2026-11-18): may a raw field own implicitly or must it be spelled unique; spike numbers in the file |

### Conflict map (never concurrent)

The overload/ctor family (NULLCTOR, PTRRANK, CTOR-VIABILITY, CONSTTWIN, CONSTPOINTEE, REFPARAM) all touch
LLVMBackend_Overloads ctor/argument selection - run them one after another. CONVOP and the operator ranking in MainListener_BinaryOperators share operator dispatch.
A3 and CHUNK share the incremental action. CACHE-KEY -> G3 share the request-cache key. N runs share the ownership
dataflow in MainListener / nulldf - one native ownership run at a time.
