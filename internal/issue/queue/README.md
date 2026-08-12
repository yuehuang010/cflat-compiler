# Issue queue: buckets

Filed 2026-08-11. The 53 active items under `internal/issue/{p1,p2,p3,ui}/` grouped by SHARED
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
| q09 | [Return-dangle and escape analysis](q09-return-dangle-escape-analysis.md) | 1 | One blocked p1 item remains; two always-wrong escapes are fixed |
| q10 | [move sinks and move spelling](q10-move-sinks-and-spelling.md) | 3 | Three fixes landed; deferred/design items remain |
| q11 | [Global and program-lifetime storage](q11-global-and-program-lifetime-storage.md) | 3 | Static-local tooling fixed; ownership semantics blocked |
| q12 | [Generics: templates and mangling](q12-generics-templates-and-mangling.md) | 6 | Five fixes landed; collisions and sizeof items remain |
| q13 | [Fixed arrays and aggregate init](q13-fixed-arrays-and-aggregate-init.md) | 4 | Five fixes landed; construction semantics remain |
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
| q09 | **SETTLED and partially fixed.** Same ratified rule as q06. The bucket file's proposal to invert to fail-closed contradicted a ruling reached after three abandoned attempts, and is corrected. The two always-wrong escape members are fixed; the third remains filed and blocked on q02. |
| q10 | **PARTIALLY FIXED.** Indirect POD move handling, forward/local alias sink inference, function-pointer allocation-alignment propagation, and lambda diagnostic wording are fixed. The move-of-borrow rule, conditional-termination guard half, and closure return-type ownership grammar remain deferred or require a language decision. |
| q11 | **PARTIALLY FIXED.** Static-local ownership-origin reporting and DWARF visibility are fixed. The three remaining global/program-lifetime ownership items remain blocked until the maintainer chooses the consume/reinitialization/destructor semantics. |
| q12 | **PARTIALLY FIXED.** Generic function registration, closure/array-view type arguments, and variadic free functions are fixed. Name-collision and `sizeof` grammar items remain deferred or coordinated with Q14. |
| q13 | **PARTIALLY FIXED.** Fixed-array rejection, nested/char-row initialization, and field default construction are fixed. Side-effect folding and owning-value replacement/read semantics remain active. |
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
