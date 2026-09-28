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
| BE | spike | p1 cold trace on P3 build | done: scratch/cold_trace_p3.md (parse 4.65 s vs clang frontend 4.04; RegisterCRecords 2.74 s for 5,656 records / 33,941 members, 5 used) |

## Bucket 0 - PRIORITY: C++ import compile time parity (p1, 2026-09-27)

Target: cflat within 10% of clang++ compiling the equivalent C++ program, cold AND warm.
Issue p1/cpp-import-compile-time-parity-with-clang. 2026-09-28 (master bd1def8e): warm 0.15x (met),
cold 1.3x with the PGO LLVM / 1.6x with the plain LLVM (torch benchmark, scratch/cmp/parity.sh). Staging: (a) warm - one pre-linked companion + one bound-surface snapshot per
import group; (b) cold - spike + plan for one clang parse per import group (persistent Sema), then build.
Gate adds scratch/cmp/parity.sh before/after numbers on an idle machine. One sample at a time:
sample 1 = torch training benchmark (scratch/cmp/train.cb); next samples only after sample 1 is at parity.

## Bucket 1 - test_libs blockers

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
| p2/cpp-libs-cache-intermittent-alias-miss-after-rebuild | full; intermittent (1 in 10 nightly-shaped runs, 2026-09-28) - cross-process entry rewrite after a build-stamp change under -j 4 |
| p3/cpp-request-cache-not-keyed-on-compiler-build | batch candidate (CxxTypeRequestCacheKey) |
| p3/cpp-wrapper-request-group-omits-type-dependencies | batch candidate (RequestGeneratedCxxWrapperUncached) |
| p3/cpp-candidate-tier-differs-cold-vs-warm | batch candidate (CandidateCxxGroupsFor) |
| p3/cpp-check-mode-cannot-repopulate-type-request-cache | full (design: --check writing the cache) |
| p3/cpp-noinc-mode-std-map-lookup-fails | full (root cause not established) |

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
| 4a4636b6 | BF | move dataflow RPO worklist: test_cpp_interop main 163 s -> 0.27 s; test.sh ~279 -> ~126-159 s |
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
