# Issue queue: buckets (round 2)

Re-created 2026-09-03 at maintainer ask ("reduce the remaining issues, p1 to p3"). Round 2 started
with 21 active items in three buckets (q01-q03); q02 and q03 retired the same day with every
member landed, and the review spin-offs they produced were re-bucketed as q04-q07 by shipping unit (also
2026-09-03, after merging four enum files into one). Same conventions as round 1 (q01-q18, retired
2026-08-31, rulings folded into `internal/fix-issue-lessons.md`): bucket files are an INDEX, not
issues; member files stay the source of truth and are deleted as fixed; delete a bucket once it
is down to two members and fold what it recorded into the survivors (or the lessons digest).

Numbering restarts at q01; round-1 buckets are cited as "round-1 qNN" to avoid collision.
Retired round-2 buckets: q02 (generics, 4 landed: 4bfa4aa 812d9bf 94ec41c), q03 (standalone,
5 landed: c56efdf 8201425 9dc0e4a 5b4947e 1fa4911; HeapAudit JIT gap ruled no-fix), q06
(generics batch, 3 landed in one commit).

## Open buckets

Buckets are now cut by HOW THEY SHIP (one worktree, one commit, one review loop), not by topic -
re-cut 2026-09-03 for throughput after a day that landed 14 issues and filed 29. A BATCH bucket
runs the fix-issue skill's batch mode (one scoped review round, no probe matrix); a full-mode
bucket runs the whole procedure. Six worktrees close 13 issues.

| # | Bucket | Mode | Closes | Ready |
|---|--------|------|--------|-------|
| q04 | BATCH: one-site lowering and diagnostic fixes (float `!=` NaN p2, bool cast, lost-count text, symbol-dump sanitize flag) | batch | LANDED e6f3521 (4 members + 2 fixed in place: brace-init bool field, interface/closure condition diagnostic; 1 fix round, 1 review round + 1 verification) - RETIRED | - |
| q01 members 10-12 | ownership alignment doors + pointer ternary join, one worktree | full | LANDED 966e3d3 (2 fix rounds + 1 verification + advisor fix in place: borrowedAddressValues_ reset, global door precise-provenance narrowing) - RETIRED | - |
| (unbucketed, filed 2026-09-04) | `p3/bool-default-argument-skips-conversion-verifier-failure` (ruling, pairs with the narrow call-arg p2), `p3/sanitizer-gated-test-legs-vacuous-in-standard-bar` LANDED 897c430 (bar is now 787), `p3/delete-of-proven-borrow-not-rejected` LANDED 455c7e3 (full mode: adds a rejection; 1 review round, wording fixed in place; open launder `T* q = move p` off `&stack` belongs to the q01 member 9 consolidation, which now has two global-storage ledgers to absorb: `globalAssignBorrowOrigin_` + `globalAssignBorrowedAddress_`) | - | 1 | ruling |
| q05 | enum as a first-class type (4 legs merged into one issue) | full | LANDED a078e02 (2 fix rounds + 1 verification; `__int128` -> APInt, aliased backing, redefinition diagnostic, dynamic-fold sign fixed in place) - RETIRED | - |
| q06 | BATCH: generics / name-resolution one-site fixes (alias -> generic base, generic-function origin, `if const` folder leaves) | batch | LANDED 139d415 (3 members + 4 fixtures retargeted in place; 1 review round + advisor fix: verbatim candidate outranks the alias hop) - RETIRED | - |
| q07 | investigation: `--sanitize=ownership --run` teardown flake p2 | full | LANDED af6814e (root cause: DIBuilder outlived the JIT-owned context under `-g`; sanitizer was incidental) - RETIRED | - |
| q01 member 9, `p3/funcptr-signature-component-lacks-declaring-scope` (ex-q06) | `NamedVariable` provenance consolidation; funcptr declaring scope - scoped-first-at-comparison-time was measured and false-rejects two legs, do not retry | full, refactors | 2 | after everything above |

All three pending rulings landed 2026-09-04 (table below). Next shipping units:
q08 (full): LANDED 680d9e8a (narrowing veto + int->bool accept at the one shared scoring point, variadic declared params, materialisation LogErrors on unscored paths, default-arg wrapper converts; needed a fewest-bool-coercions tie-break in the implicit tier; 1 review round, no amendments) - RETIRED. Review filed `p3/extern-c-narrow-param-not-enforced` (extern `u8` params lower as i32, ruling bypassed).
Maintainer notes from the q08 review: enum -> bool is accepted via the enum backing type (widens the letter of the ruling; values correct); `{f(double), f(bool)}` with an int now picks bool since int->double was never accepted; default `u8 v = 300` truncates to 44 like the declaration form (advisor accepted: the default is the parameter's declaration initializer); five new inline comments exceed 2 lines (style nit, not amended);
q09 BATCH: LANDED e0afc3b3 (view-arg `T*` -> element pointer via one decay arm; scalar return splats through the new shared `SplatScalarToVectorType`, replacing all three CreateVectorSplat sites; 1 review round, no amendments) - RETIRED. Review filed `p3/generic-view-of-pointer-arg-drops-element-star` (P<int*[]> keys as P<int[]>; full mode, moves instantiation keys).
Two agents at a time (concurrency cap).

## Suggested order

1. **q04** and **q07** in parallel - both landed 2026-09-04 (e6f3521, af6814e). Batch mode measured: 4 issues + 2 in-place fixes closed in one worktree, ~25 min wall clock from spawn to merge.
2. **q01 members 10-12** and **q05** in parallel - both landed 2026-09-04 (966e3d3, a078e02). Full mode measured: ~2h each spawn to merge, 2 fix rounds each.
3. **q06** landed 2026-09-04 (139d415), ~35 min spawn to merge; the pointer-to-view member still needs its ruling.
4. Refactors last.

## Rulings needed before the marked members start

| Item | Question | Recommendation in the file |
|------|----------|----------------------------|
| q01 `p2/unique-field-heap-array-through-move-param` | how to stop `new T[n]` adopting into `unique<T>` through a `move T*` param | RULED 2026-09-04: raw `T* p = new T[n]` STAYS legal (C compatibility), blocking is OFF. Item moves under internal/plan/delete-form-static-analysis.md (check 4): static reject where provenance is precise + runtime trap at the core ctor/reset where it is not. ON HOLD by the maintainer the same day - no work on this area until reopened. |
| q01 members 10-12 (fix/align-doors) | may a store of UNKNOWN-provenance pointer (nullptr local, `&static`, call result, borrow) into a clause-bearing `alignas` GLOBAL be rejected | advisor call 2026-09-04, applied in the commit: reject only when RHS provenance is precise and mismatched (fresh `new`, or a local with a known AllocAlignValue); unknown provenance keeps compiling. Field twin unchanged (clause-bearing fields must be `unique`). Maintainer may tighten later. |
| (ex-q06) `p3/generic-pointer-to-view-field-collapses-to-raw-pointer` | is `T*` with `T` an array view an error at instantiation, or a decay to the element pointer by design | RULED 2026-09-04: DECAY by design (`T[]` allowed in generics, `T*` -> element pointer, C-like). Probe found the real bug: `holder.p = data; holder.p[2]` reads garbage because the field indexes with pointer stride. Now a batchable codegen fix, no new diagnostic. |
| (unbucketed) `p2/narrow-param-call-arg-skips-truncation-verifier-failure` + `p3/bool-default-argument-skips-conversion-verifier-failure` | is implicit integer narrowing at a call legal | RULED 2026-09-04: NOT allowed - overload resolution rejects sub-int-to-narrower with the existing "no overload" text. Exception: integer -> `bool` at a call IS allowed (constants and non-constants), through `CoerceToBoolCondition`. Ships as one batch unit. |
| (unbucketed) `p3/simd-scalar-return-does-not-splat` | should `return` splat a scalar into `simd<T,N>` as assignment does | RULED 2026-09-04: yes - return initializes under the assignment rules (C/C++ model, each return converts independently to the declared type). Route through the assignment splat helper. Batchable. |

## Deferred - filed, not queued

Each carries its own recorded ruling or blocker. Listed so nobody re-triages them.

| Item | Why not queued |
|------|----------------|
| `p2/delete-borrow-via-named-local` | depth-2 forwarding + `function<T>` indirect only; needs an interprocedural summary pass, approach B and same-function checks prohibited |
| `p2/deref-of-moved-pointer-guard-inside-callee` | maintainer decision 2026-07-25: conditional-termination half deliberately NOT modelled; workarounds are the answer |
| `p3/uninitialized-new-array-reads` | `init` contract, `init_capacity` and its poison fill landed; raw `T* p = new T[n]` stays uninitialized by the 2026-09-04 C-compatibility ruling (no migration); only the `alloc_zeroed` optimization remains, pending allocator measurement |
| `p3/generated-code-is-roughly-twice-the-size-msvc-emits` | no root cause, investigation item; first step is a Clang-18 leg or one-function IR diff |
| `p3/no-incremental-build-and-no-up-to-date-check` | case 1 landed; case 2 is plan-level (per-import bitcode + `llvm::Linker` plumbing) |
| `p3/no-tls-in-core-network` | platform feature (Schannel / OpenSSL backends), not a bug |
| `p3/os-abstraction-process-cb-not-converted` | needs an `os.proc_*` surface design; exception stands |
| `p3/pools-no-destructor-shutdown-ordering` | ruled 2026-08-11 leave filed, not a bug |
| `p3/thread-cannot-go-raii` | ruled 2026-08-11 leave filed; blocked on deleted-copy syntax (language design) |
| `ui/edge-fill-winui`, `ui/listview-dark-column-rules` | Windows UI items, not reachable from the macOS host |

Still-open language gap carried from round 1, still unfiled: **no syntax for a deleted copy**
(blocks `Thread` RAII and any future non-copyable type).
