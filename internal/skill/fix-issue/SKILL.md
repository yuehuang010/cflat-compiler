---
name: fix-issue
description: Fix a known issue holistically in an isolated git worktree with a delegated agent - the issue is a pointer to a problem area, so the agent enumerates coverage axes and builds a pre-fix behaviour matrix before fixing - then review with an opus code-review agent until clean and merge back to master as a single-parent commit. Use when the user says "fix issue X", names a file in internal/issue/, or asks to work an issue end-to-end in a worktree.
---

# Fix Issue

End-to-end workflow: worktree -> fix agent (explore, then fix) -> review loop ->
linear merge back to master.

**The issue file is a POINTER to a problem area, not the boundary of the work.**
The recurring failure of past rounds was incomplete coverage found only in
review: the fix closed the filed spelling while a sibling spelling, a
neighbouring construct kind, or a mixed sub-case still reproduced. The fix agent
must therefore enumerate the area FIRST - axes, spellings, neighbours - build a
pre-fix behaviour matrix, and only then fix, so the first implementation pass is
already holistic and review verifies rather than discovers.

> **Git commit rule is bypassed for this skill.** CLAUDE.md's "Do not commit to
> git" does NOT apply here: invoking this skill IS the user's approval to commit.
> The workflow is built on commits (branch commits, rebase, `--ff-only`) and
> cannot work without them. Commit on the `fix/<slug>` branch freely; never
> commit directly to `master` - `master` only ever advances by fast-forward.

Args: `<issue>` (a path under `internal/issue/`, an issue slug, or a free-text
description) and optionally a tier `sonnet` | `opus` for the fix agent.
Default tier is `sonnet`.

## Step 0 - Resolve the issue and tier

- If the arg names or matches a file in `internal/issue/`, read it. That file's
  summary / repro / root cause / fix direction is the spec. Note whether the file
  is tracked (`git ls-files`): a tracked file's deletion belongs in the fix
  commit; an untracked one cannot be (the worktree will not even contain it) -
  just `rm` it from the main checkout at cleanup instead.
- If it is free text with no matching issue file, use the text as the spec and
  note that no issue file exists (one may need to be written or deleted later).
- Parse a trailing `sonnet` / `opus` token as the tier. Otherwise `sonnet`.
  Independent of the token, default to `opus` when the area is one where
  enumeration itself is the hard part - name resolution, generics
  instantiation/key space, ownership/lifetime, overload resolution - since a
  shallow Phase A there just moves the coverage gap into review.
- Confirm the working tree is clean (`git status --porcelain`). If it is not,
  stop and tell the user - do not stash their work without asking.

## Step 1 - Create the worktree

Branch name: `fix/<issue-slug>`. Worktree path: sibling of the repo.

```bash
git worktree add ../cflat-fix-<slug> -b fix/<slug>
```

`vcpkg_installed` lives outside the source tree, so the worktree builds with no
extra setup (see CLAUDE.md "Git worktrees").

## Step 2 - Fix agent

Spawn ONE agent at the resolved tier with a self-contained prompt. The prompt
must structure the work as two phases and require both in the report.

**Phase A - Explore the area and build the coverage matrix (before touching the
compiler):**

- Verify the filed repro first and report what actually happened - issue files
  have recorded wrong severities, non-reproducing repros, and wrong root causes.
  A filed root cause is a hypothesis with a citation, not a measurement.
- Treat the issue as a pointer to an AREA. Enumerate the coverage axes before
  writing any leg or any fix:
  - **Spelling axis**: every spelling that reaches the same code path - bare,
    namespace-qualified, aliased (`using X = ...`), as a generic argument,
    nested, inferred. Past fixes have been half-done along this axis twice.
  - **Syntax axis**: every way the SAME construct can be written, which is a
    different question from what type it is written on. `T x = {...}` vs
    `T x {...}`; prefix vs postfix; parenthesized vs bare receiver; the
    statement form vs the declaration form. An enumeration that covers only
    TYPES and SCOPES - struct/union/class, global/local, namespaced, `static`,
    `const`, containers, arrays, nesting - looks exhaustive and still misses a
    second grammar spelling of the same construct. Check the grammar for
    alternate spellings rather than inferring them from the source you have
    read; a filed bug has survived a fix untouched under a
    one-character-different spelling.
  - **Collision axis**: the same name declared in two scopes, plus its
    unique-name twin (the twin proves the bare spelling reaches its own key).
  - **Neighbour axis**: adjacent construct kinds that flow through the same
    machinery (e.g. a bug filed against interfaces that lives in all generic
    template kinds). Probe them - it is cheap and sets the real scope.
  - **Sub-case axis**: a deferred or filed item is often two sub-cases with
    different answers (e.g. mixed vs fully-generic). Split and test each.
- Write the matrix as a probe corpus in `scratch/` (unique file prefix) and run
  every cell against the CURRENT binary, recording verbatim pre-fix behaviour
  per cell. A leg that behaves identically before and after the fix tests
  nothing - this pre-fix recording is the one process step that has worked
  every time.
- Report the matrix: each cell's shape, its pre-fix behaviour, and whether the
  fix will change it. Cells deliberately left out of scope must be listed with
  a reason each, not silently dropped.
- **If the fix will REJECT anything, the ACCEPT-SET is a Phase A deliverable -
  produced and frozen as value legs BEFORE any guard is written.** Enumerate the
  programs in the same syntactic neighbourhood that master compiles and runs
  CORRECTLY, measure each one's value on the current binary, and commit them as
  must-still-work legs. Only then write the guard, and re-run that set after.
  False rejections are the single highest-cost failure mode in this workflow -
  they have consumed whole 3-round budgets and landed nothing more than once.
  The ordering is the point. Writing the guard first and letting the review hunt
  for what it broke puts discovery in the most expensive place, and the reviewer
  has to reconstruct the accept-set anyway - so it gets built either way, just
  later and by the wrong party. A guard whose accept-set was never enumerated is
  not ready to review. See "On guard polarity" in
  `internal/fix-issue-lessons.md` for the case history.
  Corollary: a rejection's DIAGNOSTIC must be true of the site it fires at, and
  every site added to a reject must be shown broken from the `--no-opt` IR -
  never from a probe value alone. A decimal probe cannot distinguish corrupted
  field bytes from a valid address, and a site has been added to a reject, with
  a message that was false there, on exactly that confusion.

**Phase B - Fix and test to the matrix:**

- Fix ALL cells that share the root cause, not just the filed spelling. If a
  cell genuinely needs separate work, say so explicitly per cell (it may become
  a new issue file).
- **Audit every COPY of the predicate you are changing, and report per-site.**
  Before reporting, grep for every site that asks the same question the fixed
  predicate asks - the same field comparison, the same flag test, the same
  early-out - and state for each whether it has the same defect and why it does
  or does not need the same treatment. A site left out of the report is a site
  nobody checked. This has repeatedly cost real defects - a gating condition
  fixed in one declarator path and not its twin, and a receiver-identity
  comparison fixed at one of two sites where the unfixed one turned an abort
  into a silent leak. Given as an instruction up front, agents produce a clean
  per-site audit immediately, so it is a required deliverable, not a review
  finding.
- Regression tests must cover every in-scope cell of the matrix, assert VALUES
  (never "it compiled"), and each leg must fail on the pre-fix binary for the
  reason its comment states - state the discriminator that proves the leg
  reaches the arm it names.
- **Assert the RESOURCE, not only the value, wherever the change touches
  ownership, lifetime, or destruction** - free counts, null-after-move, and an
  actual leak measurement (`leaks --atExit` on macOS). A value can be right
  while the resource is wrong: a leg asserting only values passes when the
  leaked buffer is the OLD destination, since the new value is correct either
  way. Record the leak baseline before and after and compare the numbers. Apply
  this to EVERY owning type the change can reach, not just the one being
  reasoned about - a fix that counted destructors rigorously for a pointer type
  and not at all for `string` shipped its defect in exactly that gap.
- **Sweep your own new legs for ones that cannot fail** before reporting. A leg
  like `Test("x_survived_teardown", 1, 1)` asserts `1 == 1` and can never go
  red; it reads as coverage and is worse than no leg. Any leg that would still
  pass with the fix reverted is in the same category - check by reverting.

The prompt must also contain:

- **Claims discipline.** Every "pre-existing" / "unchanged" / "not a regression"
  / "identical on both binaries" claim must ship with a MEASURED pre/post pair,
  in the exact spelling the claim is about. Asserting equivalence by inference
  from a sibling spelling is banned - two spellings of one construct have
  diverged (one genuinely unchanged, its twin going from `undef` to a corrupt
  value), and a cell reported as "now behaves exactly like its named-local
  equivalent" was leaking. Each such claim costs a full round-trip to correct
  after the fact.
- **The oracle caution.** If the fix is specified as "make X agree with Y" - a
  scope matching another scope, a new path matching an existing one - Y must be
  verified INDEPENDENTLY before it is used as the reference. When one declarator
  path was used as the oracle for another, the copied gating condition carried
  the identical hole: every check the agent could run said the two now matched,
  and they did, including in the hole. Agreement with a broken reference is
  invisible to a strategy built on agreement.
- The absolute worktree path, and an instruction to do ALL work there.
- The full issue text (summary, repro, root cause, fix direction) pasted inline.
- The relevant sections of `internal/fix-issue-lessons.md` (at minimum "On
  tests", "On guard polarity", and any section matching the issue's area), or
  an instruction to read that file from the worktree before starting.
- Repo constraints from CLAUDE.md: both copies of `ParseDeclarationSpecifiers`
  must be updated together; `LogError` / `LogErrorContext` only (no `LogWarning`,
  no `std::cout`); ASCII-only text; inline comments <= 2 lines; do not create new
  test files - extend an existing related `Test/*.cb`; any new `TypeAndValue` /
  `StructData` / `AnnotationValue` field read by an analysis must be added to the
  `--init` cache round-trip in `LLVMBackend.cpp`.
- Verification bar: `./cmake_build.sh release && ./test.sh Release` must be green
  in the worktree, AND the host example gate must pass with 0 failures
  (`bash example_mac.sh Release` on macOS, `example.bat` on Windows). The example
  gate is mandatory whenever the diff touches the compiler or `core/*.cb` -
  examples exercise core libraries (GUI, shell) that `test.sh` never compiles, and
  a diagnostics change can break them while the test suite stays green. Report the
  actual PASS/FAIL counts for both.
- Instruction to land the regression legs by extending an existing related
  `Test/*.cb` file (no new test files), then delete the `scratch/` probe corpus
  - a tracked corpus outside every test glob is run by nothing and rots.
- Instruction to COMMIT the fix on the branch as **exactly one commit** - fix,
  regression test, and issue-file deletion all in it. If the agent makes
  intermediate commits while working, it must squash them down to one before
  reporting (`git reset --soft master && git commit`). This is the one place
  committing is expected and the user has authorized it by invoking this skill.
  Still never commit to `master` directly.
- Instruction to report: the coverage matrix with pre/post behaviour per cell,
  files changed, root cause confirmed vs. issue file (with what the repro
  actually showed), test results, out-of-scope cells with reasons, and anything
  it could not resolve.

Before accepting the report, verify it against reality yourself
(`git rev-list --count master..HEAD`, `git status --porcelain`, the diff) -
agents have reported work that did not exist. A report with no coverage matrix,
or a matrix whose cells were never run pre-fix, is incomplete: send it back
before review, not into review.

**Do that verification as ONE pass, and send ONE message.** Read the whole diff,
every new or changed issue file and test comment, and run your probe matrix -
then write a single consolidated message. Do NOT stream findings as you notice
them. Streaming has cost issues several extra agent invocations each, and the
late ones are typically a comment naming a case the code never reaches or a
false measurement in an issue file - prose defects that still force a full
invocation plus a mandatory bar re-run, and that were all discoverable in one
reading.

**Skip the full bar for provably doc-only amendments.** Confirm with
`git diff --stat` that no `.h`/`.cpp`/`.cb` file is touched, and say in the
message that the bar is being skipped for that reason. A rebuild plus
`test.sh` plus the example gate is several minutes; spending it to re-prove that
a markdown edit did not change the compiler is pure overhead. Anything touching
compiler sources or `Test/` still runs the full bar, always.

If the agent fails or flails at `sonnet`, escalate ONCE to `opus` with the failure
context appended - do not retry the same tier verbatim, and do not absorb the work
into the main session.

## Step 3 - Review loop (max 3 rounds)

After the fix agent reports success, spawn a code-review agent at **opus** in the
same worktree. Prompt it to review `git diff master...HEAD` in that worktree for
correctness bugs, and for the CLAUDE.md constraints listed above. Give it the fix
agent's coverage matrix and ask it to audit the matrix, not re-derive it:

- Is any axis missing a cell - a spelling, collision twin, neighbouring
  construct kind, or sub-case that reaches the same code path but was never
  enumerated? A missing cell is a finding even if the code in the diff is
  correct.
- Does each regression leg fail on the pre-fix binary FOR THE STATED REASON
  (reaches the arm it names), not merely fail?
- For changes to anything every program flows through (guard predicates,
  overload resolution, mangling, type-flag semantics), require the differential
  corpus sweep: build the parent commit in a scratch worktree, run both binaries
  over every `.cb` in `Test/` and `example/`, and diff.
- **Hand the fix agent's probe corpus to the reviewer; do NOT have it rebuild
  one.** The fix agent leaves its corpus in `scratch/` (it is gitignored, so tell
  it not to delete it until the merge). The reviewer's job on that corpus is to
  SPOT-CHECK it - re-measure a sample, and confirm the pre/post pairs say what
  the report claims - and then spend its budget on the axes the corpus does NOT
  cover. Both parties independently building a full probe corpus is the largest
  avoidable duplication in this workflow, and the duplicated half is the half
  that finds nothing: real defects come from attacking an unprobed axis, never
  from re-running a cell the fix agent already ran.
  Weight the evidence accordingly. The whole-corpus differential sweep is the
  WEAK half for any change that adds a rejection - it structurally cannot see a
  crossing no corpus file performs, and it has certified changes that targeted
  probes then broke. Targeted crossings are the strong half. A clean sweep is
  necessary, never sufficient.

Tell it to use a unique `scratch/` file prefix (e.g. `rev_`) so it cannot
collide with the fix agent's files, to report findings as a ranked list with
file:line, and to state plainly if the diff is clean - "safe with listed fixes"
is not clean.

- If findings exist: send them back to the fix agent (same worktree; continue the
  existing agent via SendMessage so it keeps its context) and ask for fixes plus a
  re-run of the full verification bar (`./test.sh Release` and the example gate).
  Review fixes must be folded into the single
  commit with `git commit --amend`, never stacked as follow-up commits. Then
  re-review, scoping the next round to what the last round CHANGED and saying
  what not to re-verify - narrowly-scoped rounds found the worst defects.
- **Carry cosmetic findings as a punch-list into the next SUBSTANTIVE round**
  rather than spending a round-trip on each. A comment with a wrong example, an
  over-long comment, a message that mis-describes a declaration - none of these
  justify their own agent invocation and bar re-run. Accumulate them and attach
  them to the next round that has real work in it. Correctness findings and
  false claims in a tracked record still go back immediately.
- Repeat until the review is clean or 3 rounds have elapsed.
- If still not clean after 3 rounds, STOP. Do not merge. Report the outstanding
  findings and leave the worktree in place for the user.

## Step 4 - Merge back to master (single parent)

Only after a clean review and a green full verification bar (`./test.sh Release`
plus the example gate). Keep history linear - rebase, never merge-commit.

First assert the branch is exactly one commit ahead of master; if it is not,
squash before going further:

```bash
git -C ../cflat-fix-<slug> rev-list --count master..HEAD   # must print 1
```

```bash
# in the worktree
git -C ../cflat-fix-<slug> rebase master
# re-verify after rebase (skippable ONLY if the rebase was a no-op - "up to date" -
# and the full bar already ran green on this exact commit)
cd ../cflat-fix-<slug> && ./cmake_build.sh release && ./test.sh Release && bash example_mac.sh Release
# fast-forward master in the main checkout
git -C <repo> merge --ff-only fix/<slug>
```

`--ff-only` is load-bearing: it fails loudly rather than creating a merge commit.
If the rebase conflicts or the post-rebase suite fails, stop and report - do not
force anything.

## Step 5 - Clean up

- Delete the `internal/issue/<slug>.md` file if the issue is now fixed (the repo
  convention is that fixed issues are removed, not marked done). If it was
  tracked, include the deletion in the merge - if the fix agent did not delete
  it, do it before the fast-forward. If it was untracked, just `rm` it from the
  main checkout now.
- Remove the worktree and branch:

```bash
git worktree remove ../cflat-fix-<slug>
git branch -d fix/<slug>
```

- **Rebuild the main checkout at the merged HEAD and re-run the full bar there**
  (`./cmake_build.sh release && ./test.sh Release && bash example_mac.sh Release`).
  This is not redundant: the worktree verified the commit, but the main checkout's
  binary is still pre-merge, and a stale binary silently passes checks against the
  OLD compiler - any later "quick check" against it gives false confidence.

## Report

Tell the user: root cause, files changed, review rounds needed, test results
(actual counts), and the resulting `master` HEAD. Verify linearity with
`git log --oneline --graph -5` and confirm no merge commit was created.
