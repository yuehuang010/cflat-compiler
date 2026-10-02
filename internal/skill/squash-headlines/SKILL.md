---
name: squash-headlines
description: Squash the unpushed commits on master (origin/master..master) into a few topical "major headline" commits, keeping the final tree identical and rewriting old hashes in the issue queue to the grouped commits. Use when the user says "squash commits", "squash to headlines", "squash from origin", or wants local history condensed before a push.
---

# Squash to headlines

Rewrites `origin/master..master` into a handful of topical commits. Master stays linear and
keeps the same final tree. The only content change is in the last (bookkeeping) commit, where
old hashes in tracked files are rewritten to the new grouped hashes. Never pushes.

Tool: `internal/skill/squash-headlines/squash.py` (run from the repo root). It builds the new
chain with `git merge-tree` / `commit-tree`, so nothing is checked out until `--apply`.

## Steps

1. **Preconditions.** On `master`, no tracked changes; untracked files are fine. Commit any
   pending work the user wants included first. The tool fetches origin and refuses to run
   unless origin/master is an ancestor of master; if it is not, rebase first.

2. **Survey.** `git log --reverse --format='%h %<(200,trunc)%s' origin/master..master`.
   Group by topic, not by timebox. Past headlines:
   - native ownership / pointer rules
   - C++ semantics like clang (const, overload / operator ranking, layout / binding)
   - C++ import compile time
   - C interop ABI
   - issue queue bookkeeping

   Aim for 3-8 groups. Put queue / resume bookkeeping commits in one final group with
   `"bookkeeping": true`.

3. **Write the plan** to `scratch/squash_<date>/plan.json`:
   ```json
   {"groups": [
     {"title": "<headline: summary of the topic, then key items>", "commits": ["abcd1234", "..."]},
     {"title": "Issue queue bookkeeping for ... (hashes point at the grouped commits)",
      "commits": ["..."], "bookkeeping": true}
   ]}
   ```
   Every commit in the range must appear exactly once. Order inside a group does not matter;
   the tool replays each group in original order. Optional top-level
   `"reword": {"<hash prefix>": "<message>"}` replaces a commit's bullet text (fixes a landing
   that carried the wrong message). A single-commit group titled with its own subject keeps its
   message verbatim - use it for an earlier unpushed headline that should stay as is.

4. **Dry run:** `python3 internal/skill/squash-headlines/squash.py scratch/squash_<date>/plan.json`.
   It prints one line per new commit, the files whose hashes it remapped, and the diff vs master.
   That diff must touch only remapped hash text.
   - Groups are emitted greedily in plan order. When a group's commits textually depend on
     another group's, the group splits into `(1/n)` parts. Many parts means the topics are
     interleaved in the same code, so fix the plan:
     - move a dependent commit into the group it depends on;
     - merge two interleaved topics under one broader headline;
     - put independent topics first.
     Re-run each time; a dry run takes seconds. Two parts are acceptable, a scatter is not.
   - `dead end` / `tree mismatch` means stop and regroup. Never apply a plan that did not
     report a clean dry run.
   - Check the remap: `git diff master <new tip> | grep '^[-+]' | head`. Only hash text should
     change. Several references collapsing onto one grouped hash is expected.

5. **Apply:** re-run with `--apply`. This creates `backup/master-pre-squash-<date>` at the old
   master and moves master with `git reset --keep`. Verify:
   - `git log --oneline origin/master..master`
   - `git diff --quiet backup/master-pre-squash-<date> master -- . ':!internal/issue'`, which
     must succeed (identical outside the remapped files).

   The tree is unchanged, so no rebuild or suite rerun is needed.

6. **After.** Old hashes stay reachable through the backup branch, and every grouped commit
   body lists them as `(was <hash>)`, so `git log --grep <old>` finds the new home. Memory
   files and scratch logs may keep old hashes; update a memory only if it is used for lookups.
   The mapping is saved to `scratch/squash_<date>/plan.remap.json`. Do not push unless the
   user says "push".

## Notes

- Never squash with `git reset --soft` onto a moved base. It silently reverts what master
  gained (see `internal/fix-issue-lessons.md`, "On squashing a branch onto master"). The
  tool's tree check guards against this.
- Intermediate grouped commits may not build on their own. Only the final tree is verified,
  the same guarantee earlier squashes gave.
- Commit messages: the headline goes on the subject line; each old commit becomes one bullet
  (full old message, Co-Authored-By trailers dropped), followed by one trailer. Headlines
  follow the repo's style: a summary clause, a colon, then the key items.
