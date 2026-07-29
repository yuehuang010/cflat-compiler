# Interface issue queue

The tracker for the interface-related entries in `internal/issue/`. Two files already
linked `[[interface-issue-queue]]` before this existed; this is that file.

Not a separate issue - an index. Each row points at the file that owns the detail.
When an issue is fixed its file is deleted (the repo convention), so delete its row
here in the same change.

Last updated 2026-07-28.

## Resume point

- master is the `as`/`is` source-routing fix, linear, tree clean.
- Full verification re-run on macOS at that commit: **512 passed / 0 failed / 8 skipped**,
  examples **35 / 0**. (Baseline immediately before it was 510/0/8; the +2 are the new
  ternary legs in `Test/test_interface.cb`.) LSP was NOT re-run - it is Windows-only.
- Queue head is [[interface-return-dangle-defeated-by-intermediate-local]] - the LAST member
  of the `as`/`is` family still open, and now the cheapest it will ever be: the provenance
  ledger it needs already exists, so it is a lookup replacing an IR walk. Read
  [[interface-boxing-sites-not-fully-consolidated]] first - that change adds the ledger's
  second consumer and will expose two currently-inert sharp edges.
- The `as`/`is` family is otherwise DONE: routing (2 issues) and boxing guards (2 issues) are
  closed. What remains under `as` are the follow-ups those fixes surfaced, all of which are
  PLAIN-path or diagnostic-quality rather than `as` defects.
- **`fix/iface-ifconst` is SHELVED, not pending.** Branch is at `23418c2`, worktree present
  at `../cflat-fix-iface-ifconst` with its ~60-file repro corpus in `scratch/` - the most
  valuable artifact of that attempt. Read
  [[iface-ifconst-blame-attempt-shelved]] before touching it; do not restart from scratch
  and do not re-litigate the grammar facts recorded there.
- `stash@{0}: review-fixes-and-untracked-plans` is pre-existing and intact. Its contents
  have never been confirmed by the user - do not drop it.
- The old "may a user file-scope interface share a name with a core interface" product
  question is **RESOLVED** (hard error, kept and polished, shipped as `853cb87`). Do not
  reopen it.

## Closed in the 2026-07-29 session

- **`as` boxing skipped every ownership guard the plain spelling applies** - fixed, closing
  BOTH `as-boxing-skips-ownership-transfer` (all four manifestations) and
  `as-boxing-skips-pointer-shape-rejection`. `GenerateSafeCast` carried the fewest of the six
  guards, so `x as IFace` skipped ownership transfer, pointer-shape rejection, and the
  non-`move` ownership-escape rejection. Now one `BoxConcreteIntoInterface`
  (`MainListener.h:9969`) carries all of them, and the declaration-init path and the `as`
  path both route through it.
  - Prerequisite that unblocked it: the source `NamedVariable` is now plumbed into
    `ParseTypeCheckExpression` via `SoleCastOperandOf`, reusing the single-child passthrough
    idiom already in `ParseAssignmentExpressionNamed`. This was built and verified
    BEHAVIOUR-NEUTRAL before any guard was added - do that in this order if you touch it.
  - It also added the provenance ledger `interfaceBoxRecords_`, which is the prerequisite for
    [[interface-return-dangle-defeated-by-intermediate-local]] - that issue's fix direction
    has been rewritten to use it and is now wiring, not design.
  - **The change BREAKS source that was only memory-correct because the transfer was missing**
    - boxing an owning local with `as` and then still using the local is now `use of moved`,
    exactly as the plain spelling always said. The repo's own `Test/test_interface.cb`
    contained such a program and was adapted (every assertion retained, a `delete` added).
    Nothing in `core/` or `example/` was affected: every `as <Interface>` there is an
    interface-to-interface downcast, never a class-to-interface box.

## Closed in the 2026-07-28 session

- **`as` / `is` fell through to the interface-source path on any unrecognised operand** -
  fixed, closing BOTH `as-cast-pointer-ternary-operand-compiler-crash` and
  `as-cast-array-shaped-source-no-diagnostic`. They were one defect:
  `GenerateSafeCast` / `GenerateIsCheck` inferred "this is a fat pointer" from the ABSENCE
  of a concrete struct name, so a pointer `?:` phi and a decayed `T[N]` both read unrelated
  storage as {vtable,data}. Replaced with `ClassifyCastSource`, a positive routing decision;
  `Unknown` is now diagnosed rather than miscompiled. The two shapes needed DIFFERENT
  answers, which is the part worth remembering: the ternary had to be made to BOX (the plain
  spelling already worked, so rejecting it would have regressed expressiveness), while the
  array had to be REJECTED with the plain spelling's exact wording. Three review rounds; the
  residue is the two new entries below. **The severity in the ternary issue file was wrong** -
  it was recorded as a compiler crash with zero output, but the compiler never crashed:
  `--run` JITs the miscompiled program in-process, so the program's SIGSEGV looked like the
  compiler's. Verify crash claims with `-o` before believing them.

- **`as` cast of a stack value to an interface crashed the compiler** - fixed. Root cause
  was `elemType` propagation: `ParseMultiplicativeExpression` populates
  `TypedValue::elemType` only for pointer sources, so a stack class value reached
  `GenerateSafeCast` with a null `elemType`, fell through to the interface-source path,
  and `CreateExtractValue(value, {1u})` ran on a class aggregate. Stack values now join
  the statically-resolved concrete branch. Three review rounds; the fallout is the four
  `as-*` files below, which are all PRE-EXISTING gaps the fix surfaced rather than caused.
- **Named arguments ignored on the interface call path** - fixed. The interface arm never
  called `namedArgument->Identifier()`, so `VariableName` was never set and `MatchFunction`
  saw no named arguments. Fixing it made call-site index and declared-parameter index
  diverge on that path for the first time, which exposed three downstream sites that had
  silently relied on them being equal (duplicate-name crash, lambda expected-type seeding,
  positional brace-init resolution). Two further pre-existing bugs surfaced while auditing
  the fields the interface arm failed to copy: a FALSE REJECTION of legal `alignas` code on
  a `move` parameter, and a SILENT MISCOMPILE where `u8 200` through an interface widened to
  `-56`. Both fixed. Residue is [[named-arg-replay-reports-losing-candidate]],
  [[iface-slot-replay-blames-wrong-slot]], [[iface-thin-function-param-no-lowering]], and
  [[iface-arg-lambda-fnptr-type-not-propagated]].

## Open - crashes and silent miscompiles

| Issue | Severity |
|---|---|
| [[duplicate-constructor-signature-hangs-compiler]] | Hang/OOM (exit 137), no diagnostic. Namespaced classes newly route onto this path. |
| [[generic-interface-registered-as-opaque-struct]] | LLVM verifier failure + false rejections. `IFace<T>` unusable in most positions. |
| [[global-primitive-array-boxed-into-interface]] | Silent miscompile, escapes the verifier. PLAIN path, not `as`. |
| [[interface-boxing-guards-are-binding-dependent]] | Double free (exit 134). Parens or `?:` erase the binding the guards key off. |
| [[return-ternary-join-concrete-pointers-not-boxed]] | Verifier abort, no source diagnostic. PLAIN path; `as` is now better. |
| [[generic-interface-namespace-scope-limit]] | Silent miscompile. DELIBERATE scope limit of `c9acb6c`, recorded so it is not lost. |
| [[iface-thin-function-param-no-lowering]] | Module verification failure, no diagnostic. Any `function<>` interface parameter. |
| [[interface-return-dangle-defeated-by-intermediate-local]] | Dangling fat pointer, no diagnostic. Both spellings. QUEUE HEAD. |

## Open - false rejections and accept-set problems

| Issue | Severity |
|---|---|
| [[bare-interface-name-resolves-outward-before-namespace]] | Makes the documented namespace workaround awkward. |
| [[iface-ifconst-base-clause-implementor]] | Implementor inside a non-taken `if const` -> "no class implements it". |
| [[unique-array-view-accepted-as-generic-type-argument]] | Inconsistent accept set, no miscompile shown. |

## Open - diagnostic quality

| Issue | Severity |
|---|---|
| [[named-arg-replay-reports-losing-candidate]] | Reports a losing candidate's name miss instead of the real failure. |
| [[iface-slot-replay-blames-wrong-slot]] | Message names a parameter that IS declared. Same root shape as the row above; fix together. |
| [[interface-collision-message-prefix-still-basename]] | The `file(line,col):` prefix is still a bare basename. |
| [[paren-as-cast-method-call-not-parsed]] | `(x as IFoo).m()` -> `unknown function '(xasIFoo)'`. Operand-shape independent. |
| [[as-cast-unbound-pointer-shape-generic-message]] | Correctly rejected, generic wording. Struct field and LOCAL `T*[N]` only. |
| [[interface-boxing-sites-not-fully-consolidated]] | No live defect. Two open-coded sites + two inert ledger sharp edges. |

## Open - latent / no repro found

| Issue | Severity |
|---|---|
| [[core-bitcode-may-cache-bodyless-rebox-thunk]] | Unreachable today; trips when any core file reachable from `runtime.cb` gains an interface-to-interface conversion. |
| [[iface-arg-lambda-fnptr-type-not-propagated]] | No failing shape found; recorded with what was tried. |

## Open - follow-ups and shelved work

- [[iface-namespace-follow-ups]] - items 2-6 of the round-1 review of `c9acb6c`. Item 1 is
  RESOLVED (`853cb87`). Item 5 (annotation/template key split) is the one reachable only
  on the Windows `[uuid]` / `[winrt]` path.
- [[iface-ifconst-blame-attempt-shelved]] - READ BEFORE attempting the `if const` blame
  diagnostic again. A serious attempt shelved after eight review rounds / nine defects.
  See the resume point above for branch and worktree state.

## The structural theme

**Interface boxing bookkeeping was duplicated across four sites** - assignment, return,
`?:`, and `as` - each carrying a different subset of the six guards, and shapes kept falling
through the gaps. This was the single largest source of entries in this queue.

It is now PARTLY resolved: `BoxConcreteIntoInterface` (`MainListener.h:9969`) is the shared
site for the declaration-init and `as` paths and carries every guard, and it records
provenance so later checks can look a fact up instead of recovering it by walking IR.
The remaining work is tracked in [[interface-boxing-sites-not-fully-consolidated]] (two
sites still open-coded) and [[interface-boxing-guards-are-binding-dependent]] (the guards
key off a NamedVariable, so any spelling that erases the binding still slips through).

The lesson worth carrying to the next duplication: **the guards were only as good as the
information reaching them.** Every fix in this family was blocked on plumbing - the source
`NamedVariable` reaching `ParseTypeCheckExpression` - not on the guard logic itself, which
already existed and was correct. Look for the missing input before writing a new check.

A third theme, from the named-arguments work: **replay loops report the first failing
candidate rather than the relevant one**. Two entries above are that shape
([[named-arg-replay-reports-losing-candidate]], [[iface-slot-replay-blames-wrong-slot]]).
Both files agree the durable fix is a single `ScoreCandidates(probe)` helper called twice,
which also removes the desync hazard of two hand-maintained loop pairs.

A second, smaller theme, now CLOSED and worth keeping as precedent: `GenerateSafeCast` /
`GenerateIsCheck` used to decide "concrete source" by pattern-matching the operand's LLVM
type and fall through to the interface-source path on anything unrecognised. The fix was
the positive routing decision both issue files predicted, and it did close the family at
once. Two lessons transfer to the boxing consolidation above:

- **Check the plain spelling before choosing reject-vs-support.** The two fall-through
  shapes needed opposite answers, and only the plain-assignment control told us which.
- **A guard is only as good as the shapes that can reach it.** Parity with the plain
  spelling was achieved for six source shapes and missed for two
  ([[as-cast-unbound-pointer-shape-generic-message]]) purely because a GEP-derived source
  has no storage key to look up. Provenance recorded AT the boxing site would not have had
  that failure mode - which is the argument for the consolidation, made concrete.

## Adjacent - found during interface reviews, not interface bugs

[[constructor-discriminator-inconsistent-name-only-sites]],
[[array-view-params-unconditionally-noalias]],
[[expect-error-leaves-outer-nullcond-block-unterminated]].

## Working notes from the fix-issue rounds

Accumulated across the interface sessions. These are the notes that changed an outcome
more than once; keep them with the queue rather than in any one issue file.

**On reviews**

- A review round found a CONFIRMED defect in nearly every round of this work. Never skip
  them. Reviews repeatedly caught the fix agent's REASONING while its code was fine.
- Never let the agent that wrote a fix be the only one to hunt for its consequences.
  Rounds 6 and 7 of the `if const` attempt each INTRODUCED the next defect while fixing
  the previous one, and self-review missed both times.
- **When an agent cites a justification, check it still holds AFTER the change it is
  justifying.** The shape-8 defect: "the Mark site feeds only `uncertainInterfaceImpls`,
  which can only weaken a proof" was true when written and false after its own edit.
  Over-broad candidate sets are SAFE for suppression and UNSAFE for blame.
- Point reviewers at the TRUE master binary (`x64/Release/cflat`); make them rebuild BOTH
  sides and verify the master binary's identity themselves.
- **Verify the PROOF, not just the answer.** One proof drove both binaries with `--check`,
  was vacuous, and still reported the right conclusion. Demand real `-o` codegen.
  (Correction: `--check` DOES reach the zero-implementor rebox diagnostic - the blanket
  claim that it never reaches rebox finalization is too strong.)
- Make reviewers state how they validated their own harness is non-vacuous. One reviewer
  ran its classifier against known-differing files in both directions first, and caught
  that 2 of its 3 real diffs were HIDDEN because `expect_error` output looks like a
  diagnostic on both sides.

**On agent reports**

- Agents have reported work that did not exist: one returned a status update as if it had
  implemented the feature (worktree had 0 commits, 0 modified files); one claimed a review
  round "found the correctness core clean" while that review was still RUNNING. **After
  every agent report, check `git rev-list --count master..HEAD`, `git log`,
  `git status --porcelain`, and the diff yourself before believing any of it.**
- An agent that delegates can orphan a child that keeps writing to the worktree with no
  one watching. Detect that before spawning a replacement, or two writers corrupt the
  same files.
- Counterexample worth trusting: an agent reporting a bug ITS OWN tests caught is doing
  real work. Fabricated reports do not contain self-inflicted findings.

**On the code**

- **`LogError` THROWS - treat it as a control-flow edge.** Three bugs traced to state or
  an IR bracket left open on the unwind path. Brief every fix agent on it.
- A "this is dead code, so it needs no handling" justification is a deferred bug. Make the
  agent prove the grammar constraint it depends on.
- Before tightening a name-only discriminator, sweep `core/` and `example/` first. A
  name-only rejection once turned `int C()` in a lock group - which master compiles and
  runs correctly - into a hard error with a factually false message, and **the suite could
  not see it: no in-repo `.cb` used the construct.**

**On tests and docs**

- **A test that pins a PATH in `expect_error` breaks on the other platform.**
  `ShortenDefSiteForDisplay` returns native separators, deliberately. Pin the
  basename + (line,col) TAIL only: separator- and cwd-agnostic, still a loud tripwire.
  `expect_error` is a plain `.find()` substring check (`LLVMBackend.h:1201`).
- **Docs must not sell an unreachable guard as the safety story.** `--init-clear`'s four
  safety guards are provably dead code today; the doc called them "deliberately defensive"
  and was rewritten to state the real contract. Apply such a correction to the COMMIT
  MESSAGE too, since that outlives the doc.
- Bugs needing 2+ `expect_error` legs in ONE function are a recurring blind spot.

**On process**

- Issue files can be wrong: one repro did not reproduce as written, another described a
  fault milder than reality. Have the fix agent verify the repro FIRST and report what it
  actually saw.
- When an agent proposes diverging from the issue file's fix direction, treat it as a RISK.
- Tell agents to use repo-root `scratch/` and never run `git stash`.
