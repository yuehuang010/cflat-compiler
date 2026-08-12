# Issue queue: buckets

Filed 2026-08-11. The 47 active items under `internal/issue/{p1,p2,p3,ui}/` grouped by SHARED
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
| q08 | for-in loop variable | `3a663e1` |
| q11 | Global, program-lifetime and static storage | this commit |
| q09 | Return-dangle and escape analysis | this commit |

## Suggested order

| # | Bucket | Items | Why here |
|---|--------|-------|----------|
| q10 | [move sinks and move spelling](q10-move-sinks-and-spelling.md) | 3 | Three fixes landed; deferred/design items remain |
| q12 | [Generics: templates and mangling](q12-generics-templates-and-mangling.md) | 4 | UNBLOCKED - collisions ruled a hard error; `Test/test_generics.cb` may be renamed |
| q13 | [Fixed arrays and aggregate init](q13-fixed-arrays-and-aggregate-init.md) | 4 | Five fixes landed; construction semantics remain |
| q14 | [Parser and expression grammar](q14-parser-expression-grammar.md) | 9 | Constructor and sizeof type parsing fixes landed; postfix gaps remain |
| q15 | [Lambdas, closures, funcptr typing](q15-lambdas-closures-funcptr-typing.md) | 3 | Callable typing gaps and null/design cases remain |
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
| q09 | **SETTLED and partially fixed.** Same ratified rule as q06. The bucket file's proposal to invert to fail-closed contradicted a ruling reached after three abandoned attempts, and is corrected. The two always-wrong escape members are fixed; the third is now UNBLOCKED - its q02 prerequisite landed in `b911ccc` and it was skipped on a stale blocker. |
| q10 | **PARTIALLY FIXED; the grammar half is now RULED.** Closure return types will accept `move` and `alias` (not `unique`) - see `p2/lambda-return-type-cannot-be-spelled-move-or-alias`. Indirect POD move handling, forward/local alias sink inference, function-pointer allocation-alignment propagation, and lambda diagnostic wording are fixed. The move-of-borrow rule (deferred until `list`/`dictionary`/btree settle) and the conditional-termination guard half remain deferred. |
| q11 | **RULED 2026-08-11, IMPLEMENTED, bucket closed.** Static-local ownership-origin reporting and DWARF visibility were already fixed. A spike (`scratch/uniqglobal/`) then showed the `unique T*` arm ALREADY implements the Rust model and struct globals implement the C++ one, so the maintainer ruled the Rust model for both: owning globals stay legal, implicit consume is an error, explicit `move` re-initializes, and NOTHING at global/static scope is destructed at exit. The core-globals worry was unfounded: `globalDtorOrder_` was already gated on `!currentSourceIsCore_`, so point 4 only ever affected USER globals. All three members are fixed and their files deleted. |
| q12 | **RULED 2026-08-11 and UNBLOCKED.** Generic function registration, closure/array-view type arguments, variadic free functions, and generic `sizeof` operands are fixed. The two name-collision items were blocked only by `Test/test_generics.cb`'s deliberate `Container<T>` collision; the maintainer authorized renaming the interface leg, so a same-name generic struct/interface collision becomes a hard `LogError` at the second declaration. |
| q13 | **PARTIALLY FIXED.** Fixed-array rejection, nested/char-row initialization, and field default construction are fixed. Side-effect folding and owning-value replacement/read semantics remain active. |
| q17 | **SETTLED as "leave filed".** Both stay design deferrals, neither is a bug. Direction if ever pursued is a non-bitwise-copyable `Thread`, but that is BLOCKED: CFlat has no syntax for a deleted copy. Pool quiescence-as-typestate was not selected. |

All six are now answered. Two of them (q06, q09) turned out to need no decision at all - the repo
had already ratified the rule and the bucket files had wrongly re-opened it. That is the failure
mode to watch for when writing these files: a summary that reads as an open question when a
tracked record already contains the answer.

Still-open language gaps surfaced by this triage, neither filed as its own issue yet:

- **No syntax for a deleted copy** (blocks q17's `Thread` direction, and any future
  non-copyable type).
- ~~**`p3/interface-boxing-keyed-on-source-binding` is a prerequisite**~~ - RESOLVED. It landed in
  `b911ccc` (q02) and its file is deleted, so q09's p1 member is actionable. The q09 round skipped
  it on this blocker AFTER the blocker had already cleared: re-check a named prerequisite against
  `git log` before deferring on it.

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
- q08 and q11 are complete. q09 and q10 touch ownership but in narrow, well-fenced paths; run
  them singly rather than alongside unrelated ownership work.
