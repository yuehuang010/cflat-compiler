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
5 landed: c56efdf 8201425 9dc0e4a 5b4947e 1fa4911; HeapAudit JIT gap ruled no-fix).

## Open buckets

Buckets are now cut by HOW THEY SHIP (one worktree, one commit, one review loop), not by topic -
re-cut 2026-09-03 for throughput after a day that landed 14 issues and filed 29. A BATCH bucket
runs the fix-issue skill's batch mode (one scoped review round, no probe matrix); a full-mode
bucket runs the whole procedure. Six worktrees close 13 issues.

| # | Bucket | Mode | Closes | Ready |
|---|--------|------|--------|-------|
| q04 | BATCH: one-site lowering and diagnostic fixes (float `!=` NaN p2, bool cast, lost-count text, symbol-dump sanitize flag) | batch | 4 | now |
| q01 members 10-12 | ownership alignment doors + pointer ternary join, one worktree | full | 3 | now |
| q05 | enum as a first-class type (4 legs merged into one issue) | full | 1 | now |
| q06 | BATCH: generics / name-resolution one-site fixes (alias -> generic base, generic-function origin, `if const` folder leaves) | batch | 3 | now (enum leaf after q05) |
| q07 | investigation: `--sanitize=ownership --run` teardown flake p2 | full | 1 | now, needs a Debug build |
| q01 member 9, q06 "not in the batch" refactor | `NamedVariable` provenance consolidation; funcptr declaring scope | full, refactors | 2 | after everything above |

Rulings block 4 more (table below). Two agents at a time (concurrency cap).

## Suggested order

1. **q04** and **q07** in parallel: q04 is the cheapest close of 4 (one of them p2); q07 is an
   investigation whose wall clock is mostly a Debug build and repeated runs, so it overlaps well.
2. **q01 members 10-12** and **q05** in parallel: both full mode, disjoint files.
3. **q06** after q05 (the enum leaf); the pointer-to-view member joins it if ruled by then.
4. Refactors last.

## Rulings needed before the marked members start

| Item | Question | Recommendation in the file |
|------|----------|----------------------------|
| q01 `p2/unique-field-heap-array-through-move-param` | how to stop `new T[n]` adopting into `unique<T>` through a `move T*` param | RULED 2026-09-03 in principle: block `new T[n]` binding to bare `T*`; maintainer weighing blast radius (~800 sites) and the `unique T[3]` reading before it starts. Option 4 (runtime trap) is off. |
| q06 `p3/generic-pointer-to-view-field-collapses-to-raw-pointer` | is `T*` with `T` an array view an error at instantiation, or a decay to the element pointer by design | error, consistent with LANGUAGE.md "pointer-to-array-view is not a valid type"; reuse the member-3 attribution |
| (unbucketed) `p2/narrow-param-call-arg-skips-truncation-verifier-failure` | is implicit integer narrowing at a call legal | either answer is fine; the file leans to "legal, truncate like assignment" so `takeU8(-w)` compiles again. Verifier dump must go either way. |
| (unbucketed) `p3/simd-scalar-return-does-not-splat` | should `return` splat a scalar into `simd<T,N>` as assignment does | yes, route through the assignment splat helper |

## Deferred - filed, not queued

Each carries its own recorded ruling or blocker. Listed so nobody re-triages them.

| Item | Why not queued |
|------|----------------|
| `p2/delete-borrow-via-named-local` | depth-2 forwarding + `function<T>` indirect only; needs an interprocedural summary pass, approach B and same-function checks prohibited |
| `p2/deref-of-moved-pointer-guard-inside-callee` | maintainer decision 2026-07-25: conditional-termination half deliberately NOT modelled; workarounds are the answer |
| `p3/uninitialized-new-array-reads` | `init` contract, `init_capacity` and its poison fill landed; what remains is the 800-site `T* p = new T[n]` migration, blocked on the hidden-length ruling ([[raw-array-count-desugar-direction]]), and the `alloc_zeroed` optimization pending allocator measurement |
| `p3/generated-code-is-roughly-twice-the-size-msvc-emits` | no root cause, investigation item; first step is a Clang-18 leg or one-function IR diff |
| `p3/no-incremental-build-and-no-up-to-date-check` | case 1 landed; case 2 is plan-level (per-import bitcode + `llvm::Linker` plumbing) |
| `p3/no-tls-in-core-network` | platform feature (Schannel / OpenSSL backends), not a bug |
| `p3/os-abstraction-process-cb-not-converted` | needs an `os.proc_*` surface design; exception stands |
| `p3/pools-no-destructor-shutdown-ordering` | ruled 2026-08-11 leave filed, not a bug |
| `p3/thread-cannot-go-raii` | ruled 2026-08-11 leave filed; blocked on deleted-copy syntax (language design) |
| `ui/edge-fill-winui`, `ui/listview-dark-column-rules` | Windows UI items, not reachable from the macOS host |

Still-open language gap carried from round 1, still unfiled: **no syntax for a deleted copy**
(blocks `Thread` RAII and any future non-copyable type).
