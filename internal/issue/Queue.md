# C++ interop / bridge fix queue

Built 2026-09-27 from the open `internal/issue/` files that concern C++ interop and the CFlat <-> C++
bridge. Native-only issues are not in the queue. Workflow is `internal/skill/fix-issue/SKILL.md`
(full mode by default, batch mode only where marked). The main session owns this file: agents never
edit it (see the worktree-agents rule). A landed row moves to LANDED with its hash, and its issue file
is deleted in the fix commit.

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

## In flight

| Row | Branch | Issue(s) | Notes |
|-----|--------|----------|-------|
| AM3 | fix/inherited-static | p3/cpp-inherited-static-members-and-member-operators-not-found | Codex x2 stalled; opus finishing (segfault regression in err_cpp_struct_tpl_arg_incomplete_list, eigen_03 Identity). Unblocks eigen_03, and part of eigen_04 |
| AP | fix/union-bitfield | p2/c-bitfield-union-access-and-enum-bitfield | in review (2410ac11, cache 114) |
| AQ | fix/mt-nontpl | p2/cpp-member-template-vs-nontemplate-overload-ranking | wrong overload when a member template sits beside a non-template |

## Bucket 1 - test_libs blockers (first)

The remaining DISABLED cases in `test_libs/`. Each landing removes its DISABLED marker.

| Issue | Blocks | Mode | Notes |
|-------|--------|------|-------|
| p3/cpp-operator-returning-unrequested-specialization-not-bound | eigen_04 (with AM2) | full, opus-tier area | lazy refused-member retry (0911b337); start after AM2 lands |
| plan converting-constructors (`internal/plan/converting-constructors.md`) | eigen_05 | plan, full | ruled 2026-09-26; largest item, needs its own staging |
| p3/cpp-incremental-retry-after-failed-parse-crashes | torch latent (t14-t16, t31 cold retries) | full | ModuleBuilder / IncrementalAction GenModule; known trigger closed, latent crash |

## Bucket 2 - pointer argument proof at C++ calls (p2, wrong value)

Same root area: `IsProvenPrimitiveSinglePointerArg` / `CompareUpconvert` / `SelectCxxConstructor` accept
or mis-score a pointer argument against a non-pointer C++ parameter. Governed by the 2026-09-26
"pointer is not a number" ruling. Full mode (overload resolution); split into two runs.

| Run | Issue | Kind |
|-----|-------|------|
| 2a | p2/cpp-pointer-argument-binds-bool-reference-directly | wrong value |
| 2a | p2/cpp-primitive-pointer-binds-class-reference-by-reinterpretation | wrong value |
| 2a | p2/cpp-pointer-argument-into-int-parameter-fails-verification | verifier failure -> compile error |
| 2a | p2/cpp-spelled-constructor-ignores-argument-pointer-depth | wrong value (2 sites) |
| 2a | p3/cpp-template-bool-parameter-accepts-pointer | wrong acceptance |
| 2b | p3/cpp-pointer-argument-prefers-char-pointer-over-converting-ctor | wrong overload |
| 2b | p3/cpp-single-level-pointer-unproven-at-call-argument | refusal gap (3 shapes) |
| 2b | p3/cpp-operator-address-of-operand-not-converted | refusal gap |

2a first: it closes the wrong-value holes. 2b then widens acceptance on the proven-pointer path.

## Bucket 3 - temporary lifetime and unwind at the bridge (leak / UAF)

Ownership/lifetime work: full mode, opus-tier area, one issue per run.

| Issue | Kind |
|-------|------|
| p3/cpp-free-wrapper-returns-reference-into-own-frame | UAF (std.min/max on rvalue), one site: RequestCxxFreeFunction |
| p3/cpp-ctor-thunk-brace-argument-backing-array-dangles | dangling backing array, one site: RequestCxxBraceConstructor |
| p3/coalesce-arm-temp-cxx-borrow-and-return-flush-gaps | leak + early free, 3 sub-bugs near DropRetainedJoinArmPtrTemps |
| p3/cpp-unwind-cleanup-gaps-ctor-new-temporaries | follow-up list after d59df39b / cpp-unwind2 |
| p3/cpp-new-remaining-allocator-gaps | follow-up list after cpp-class-operator-new (5 items) |

## Bucket 4 - C++ constructor call parity

Constructor calls should convert and rank like C++ function calls. Rows marked batch are candidates for
one batch-mode run of up to 4 members, once the agent confirms each is one site.

| Issue | Mode |
|-------|------|
| p3/cpp-constructor-call-accepts-implicit-narrowing | full (adds a rejection) |
| p3/cpp-ambiguous-default-ctor-picks-nullary | full (adds a rejection) |
| p3/cpp-trivially-copyable-private-default-ctor-silently-zeroed | full (adds a rejection) |
| p3/cpp-ctor-expression-position-const-ref-copies-lvalue | full (2 bugs) |
| p3/cpp-ctor-nullptr-t-param-refused | batch candidate |
| p3/cpp-unscoped-enum-to-double-param-refused | batch candidate |
| p3/cpp-nonconst-default-refused-when-an-argument-is-a-string-literal | batch candidate (site: LLVMBackend_Overloads.cpp ~3318) |
| p3/cpp-converting-ctor-raw-array-source-refused-implicit | batch candidate; overlaps the converting-constructors plan, so land it with Bucket 1 if that starts first |

The first three add rejections, so they can go as one full-mode run over the ctor selection path.

## Bucket 4b - rulings landed 2026-09-27

| Issue | Mode | Ruling |
|-------|------|--------|
| p3/cpp-assignment-through-const-reference-result-accepted | full (adds a rejection; one site in the assignment handler) | refuse: CFlat exposes no const, but respects C++ const internally |
| p4/localize-relayed-clang-diagnostics | batch candidate | no translation; every clang-sourced message gets a `clang: ` prefix - finish coverage of relay sites |
| p3/cpp-sink-refusal-cause-lost-under-harvest-suppression | full (diagnostic surfacing) | relayed clang causes follow the `clang: ` prefix rule |

## Bucket 9 - plan: CFlat structs with nontrivial C++ fields (ruled A, 2026-09-27)

`internal/plan/cflat-struct-nontrivial-cxx-fields.md`. Phase 0 (stopgap refusal) is small and can go
early; phases 1-4 are staged, one phase per branch. Absorbs p2/cpp-struct-list-field-relocated-bitwise,
p3/cpp-prvalue-to-cflat-byvalue-param-extra-copy, p3/cpp-typed-local-extra-copy-from-param-and-ctor-arms.

## Bucket 5 - C++ operators

| Issue | Notes |
|-------|-------|
| p3/cpp-pointer-left-free-operator-not-dispatched | pointer LEFT operand, C++ class right |
| p3/cpp-chained-string-operator-fold-refused | `s + a + b` on std.string; same dispatch path as the row above |

One full-mode run for both.

## Bucket 6 - header import, layout, declaration conflicts

| Issue | Mode |
|-------|------|
| p3/cpp-no-unique-address-with-bitfields-or-anonymous-member-refused | full (PackBitfields; related to AP) - start after AP lands |
| p3/cpp-class-vs-class-template-same-name-conflict-not-diagnosed | full (adds a rejection; CheckCxxNamespaceConflicts, one site) |
| p3/cast-of-scoped-enumerator-does-not-fold-in-template-argument-position | batch candidate (NTTP integer folder) |

## Bucket 7 - request cache and incremental infrastructure

Correctness of the C++ request cache across cold/warm/rebuild. No language surface. Pair with a
cold + warm torch run in the gate.

| Issue | Mode |
|-------|------|
| p3/cpp-request-cache-not-keyed-on-compiler-build | batch candidate (CxxTypeRequestCacheKey) |
| p3/cpp-wrapper-request-group-omits-type-dependencies | batch candidate (RequestGeneratedCxxWrapperUncached) |
| p3/cpp-candidate-tier-differs-cold-vs-warm | batch candidate (CandidateCxxGroupsFor) |
| p3/cpp-check-mode-cannot-repopulate-type-request-cache | full (design: --check writing the cache) |
| p3/cpp-noinc-mode-std-map-lookup-fails | full (root cause not established) |
| p3/cpp-interop-fixtures-gate-ci-wall-time | perf; profile first, no fix before a measurement |

## Bucket 8 - std library coverage

| Issue | Notes |
|-------|-------|
| cppinterop/stream-classes-no-callable-destructor | destructor fixed, two rungs remain (CClangExtract, BuildCxxVirtualThunks) |
| cppinterop/stream-open-instantiation-error | clang errors inside the generated body; partly root-caused |
| cppinterop/std-header-coverage-spike | survey/index; re-run after buckets 1-5 to refresh the gap list |

## Parked - needs a maintainer ruling before any work

| Issue | Question |
|-------|----------|
| p4/string-functional-construction-spelling | p4: spelling ruling needed |

## Suggested order

1. In flight: AM2, AP, AQ.
2. Bucket 1: operator-returning-unrequested-specialization (after AM2), then incremental-retry crash.
3. Bucket 2a (p2 wrong values), then 2b.
4. Bucket 3, one at a time (the two one-site UAFs first).
5. Buckets 4 and 5 in parallel (disjoint paths: ctor selection vs operator dispatch).
6. Bucket 6, Bucket 7 batch, then the converting-constructors plan (eigen_05) as its own staged effort.
7. Bucket 8 and a coverage re-survey.

At most 3 implementers at once. Rows touching the same function (2a/2b, 4/Bucket 1 converting ctors,
6/AP) never run concurrently.

## LANDED

| Hash | Row | Issue(s) |
|------|-----|----------|
