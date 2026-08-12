# Issue queue: buckets

Filed 2026-08-11. The 69 active items under `internal/issue/{p1,p2,p3,ui}/` grouped by SHARED
ROOT CAUSE, so a round of work fixes a family rather than one symptom. Each issue appears in
exactly one bucket; cross-bucket relatives are named under "Adjacent" inside each file.

These files are an INDEX, not issues. They own no repro of their own - the member files stay the
source of truth and are deleted individually as they are fixed. Delete a bucket file when its
member list is empty.

## Completed

| # | Bucket | Landed |
|---|--------|--------|
| q01 | Owned-temp ledger accounting | `c7996f2` |
| q02 | Join-arm classification | `b911ccc` |
| q03 | Namespace and alias resolution | `dfa443a` |
| q04 | Diagnostics wording and attribution | `9062709` |
| q05 | Unique/owning assignment arm | `3f1bee6` |
| q06 | Borrow provenance lost across a hop | `427e076` |
| q07 | Facts not retired on rebind | `f3a135f` |
| q08 | for-in loop variable | this commit |

## Suggested order

| # | Bucket | Items | Why here |
|---|--------|-------|----------|
| q09 | [Return-dangle and escape analysis](q09-return-dangle-escape-analysis.md) | 3 | |
| q10 | [move sinks and move spelling](q10-move-sinks-and-spelling.md) | 6 | |
| q11 | [Global and program-lifetime storage](q11-global-and-program-lifetime-storage.md) | 4 | |
| q12 | [Generics: templates and mangling](q12-generics-templates-and-mangling.md) | 11 | Disjoint from ownership; parallelizable |
| q13 | [Fixed arrays and aggregate init](q13-fixed-arrays-and-aggregate-init.md) | 9 | Disjoint; parallelizable |
| q14 | [Parser and expression grammar](q14-parser-expression-grammar.md) | 11 | Disjoint; parallelizable |
| q15 | [Lambdas, closures, funcptr typing](q15-lambdas-closures-funcptr-typing.md) | 5 | |
| q16 | [Codegen folding and determinism](q16-codegen-folding-and-determinism.md) | 4 | |
| q17 | [Concurrency and RAII resources](q17-concurrency-and-raii-resources.md) | 3 | Two are design decisions, not bugs |
| q18 | [Platform, C interop, UI](q18-platform-c-interop-and-ui.md) | 8 | Disjoint; parallelizable |

## Design rulings

Triage session 2026-08-11. Six buckets were flagged as needing a maintainer decision before any
fix work. Status:

| Bucket | Ruling |
|--------|--------|
| q06 | **SETTLED - no decision was needed.** The repo had already ratified it: unknown ACCEPTS. The bucket file's proposal to make unknown reject was wrong and is corrected. The interface half of the p1 item is CLOSED, not open. |
| q08 | **SETTLED and fixed.** The `for-in` loop variable is a BORROW of the element; assignment writes through to the container element. The landed implementation also rejects overwriting the borrowed collection storage and deep-copies returned values. |
| q09 | **SETTLED - no decision was needed.** Same ratified rule as q06. The bucket file's proposal to invert to fail-closed contradicted a ruling reached after three abandoned attempts, and is corrected. Two of the three members are always-wrong shapes that can be rejected without touching the polarity; the third is blocked on q02. |
| q11 | **BLOCKED.** Needs a design discussion: a global's lifetime cannot easily be proven, so a destructor cannot easily be run for it. Do not start this bucket. |
| q13 | **SETTLED.** Multi-dim fixed arrays: nested braces only (`{{1,2,3},{4,5,6}}`), plus string rows for char arrays (`{"ab","cd"}`). A flat list for a multi-dim array is an ERROR naming the expected shape. |
| q17 | **SETTLED as "leave filed".** Both stay design deferrals, neither is a bug. Direction if ever pursued is a non-bitwise-copyable `Thread`, but that is BLOCKED: CFlat has no syntax for a deleted copy. Pool quiescence-as-typestate was not selected. |

All six are now answered. Two of them (q06, q09) turned out to need no decision at all - the repo
had already ratified the rule and the bucket files had wrongly re-opened it. That is the failure
mode to watch for when writing these files: a summary that reads as an open question when a
tracked record already contains the answer.

Still-open language gaps surfaced by this triage, neither filed as its own issue yet:

- **No syntax for a deleted copy** (blocks q17's `Thread` direction, and any future
  non-copyable type).
- **`p3/interface-boxing-keyed-on-source-binding` is a prerequisite**, not just an adjacent item -
  q09's p1 member cannot be fixed until it lands. Tracked in q02.

### The governing rule for the guard family (q06, q09, and every future proposal in the area)

**Unknown ACCEPTS. A false rejection is a blocker; a missed dangle is today's behaviour.**

Arrived at after three abandoned attempts that all rejected legal programs. What IS permitted is
rejecting a shape that is ALWAYS wrong - one no correct program has. Rejecting an unknown shape is
not. Two specific relaxations have already been BUILT, MEASURED, and reverted for false-rejecting
code in `Test/test_move.cb`; they are named in the q06 file so nobody retries them.

## Scheduling

- **Completed chain** q01 -> q02 -> q05 -> q06 -> q07. They all touched the same ownership code in
  `LLVMBackend.h` / `MainListener.h` and were landed sequentially.
- **Parallel-safe** against that chain and against each other: q03, q04, q12, q13, q14, q18.
- q08 is complete. q09, q10, and q11 touch ownership but in narrow, well-fenced paths; run them
  singly rather than alongside unrelated ownership work.
