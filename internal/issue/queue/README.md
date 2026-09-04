# Issue queue: buckets (round 2)

Re-created 2026-09-03 at maintainer ask ("reduce the remaining issues, p1 to p3"). The 21 active
items under `internal/issue/{p2,p3}/` (p1 is empty) split into 12 ACTIONABLE items in three
buckets and 9 DEFERRED items that stay filed but are not queued. Same conventions as round 1
(q01-q18, retired 2026-08-31, rulings folded into `internal/fix-issue-lessons.md`): bucket files
are an INDEX, not issues; member files stay the source of truth and are deleted as fixed; delete a
bucket once it is down to two members and fold what it recorded into the survivors.

Numbering restarts at q01; round-1 buckets are cited as "round-1 qNN" to avoid collision.

## Open buckets

| # | Bucket | Members | Ready now | Needs ruling |
|---|--------|---------|-----------|--------------|
| q01 | Ownership codegen: counted move pointers, slot moves, unique remnant | 2 open (members 1, 3, 4, 5, 6, 7, 8 landed; member 2 blocked on the hidden-length ruling; member 9 refactor last) | 3 + 1 refactor | 1 |
| q02 | Generics and name resolution: instantiation timing and attribution | 4 | 3 + 1 refactor | 0 |
| q03 | Standalone correctness fixes, no shared root | 1 open (promotion landed c56efdf, ternary ownership error landed 8201425; HeapAudit JIT gap ruled no-fix) | 1 | 2 |

## Suggested order

1. **q01** first: the two original p2 memory-safety items landed 2026-09-03; next are the two p2
   siblings their reviews surfaced (members 5 and 6, same count/alignment lookup class) and the
   ruled-but-unlanded alignment cleanup (member 4).
2. **q02**: all pre-existing, all with a verified or near-verified root cause in the scanner's
   queue-at-scan-time path; one round, one test file (`Test/test_generics.cb`).
3. **q03**: independent one-file fixes; run in parallel with q02 if capacity allows (disjoint
   files).

Refactor members (consolidate-borrow-provenance, centralize-scoped-registry) go LAST in their
bucket, after the bug members, so they refactor code that has stopped moving.

## Rulings needed before the marked members start

| Item | Question | Recommendation in the file |
|------|----------|----------------------------|
| q01 `p2/unique-field-heap-array-through-move-param` | how to stop `new T[n]` adopting into `unique<T>` through a `move T*` param | RULED 2026-09-03 in principle: block `new T[n]` binding to bare `T*`; maintainer weighing blast radius (~900 sites) and the `unique T[3]` reading before it starts. Option 4 (runtime trap) is off. |

## Deferred - filed, not queued

Each carries its own recorded ruling or blocker. Listed so nobody re-triages them.

| Item | Why not queued |
|------|----------------|
| `p2/delete-borrow-via-named-local` | depth-2 forwarding + `function<T>` indirect only; needs an interprocedural summary pass, approach B and same-function checks prohibited |
| `p2/deref-of-moved-pointer-guard-inside-callee` | maintainer decision 2026-07-25: conditional-termination half deliberately NOT modelled; workarounds are the answer |
| `p3/generated-code-is-roughly-twice-the-size-msvc-emits` | no root cause, investigation item; first step is a Clang-18 leg or one-function IR diff |
| `p3/no-incremental-build-and-no-up-to-date-check` | case 1 landed; case 2 is plan-level (per-import bitcode + `llvm::Linker` plumbing) |
| `p3/no-tls-in-core-network` | platform feature (Schannel / OpenSSL backends), not a bug |
| `p3/os-abstraction-process-cb-not-converted` | needs an `os.proc_*` surface design; exception stands |
| `p3/pools-no-destructor-shutdown-ordering` | ruled 2026-08-11 leave filed, not a bug |
| `p3/thread-cannot-go-raii` | ruled 2026-08-11 leave filed; blocked on deleted-copy syntax (language design) |

Still-open language gap carried from round 1, still unfiled: **no syntax for a deleted copy**
(blocks `Thread` RAII and any future non-copyable type).
