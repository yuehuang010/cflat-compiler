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

**Phase B - Fix and test to the matrix:**

- Fix ALL cells that share the root cause, not just the filed spelling. If a
  cell genuinely needs separate work, say so explicitly per cell (it may become
  a new issue file).
- Regression tests must cover every in-scope cell of the matrix, assert VALUES
  (never "it compiled"), and each leg must fail on the pre-fix binary for the
  reason its comment states - state the discriminator that proves the leg
  reaches the arm it names.

The prompt must also contain:

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
