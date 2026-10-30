---
name: rule-issues
description: Work through the "needs ruling" items parked in internal/issue/ - rule the ones existing direction already decides, put the rest to the maintainer as concrete options with a recommendation, and record every ruling in the issue file itself. Use when the user says "answer the rulings", "rule the open questions", "work through the ruling list", or a resume file / Queue.md carries a NEEDS RULING list.
---

# Rule the open issues

A ruling is a maintainer decision on language surface or semantics that an issue needs before it can be
fixed. Most of them are already decided by an earlier ruling or by the design direction; the main session
answers those itself and only puts the genuinely open ones to the maintainer. Every ruling lands in the
issue file, nothing else: `internal/fix-issue-lessons.md` is for lessons, not rulings, and auto-memory never
holds one (the repo is the record). Nothing is committed by this skill (do-not-commit rule).

## Steps

1. **Collect the list.** Sources, in order: the resume file the user points at (`scratch/resume_*.md`
   "Rulings needed"), `internal/issue/Queue.md` rows saying NEED(S) RULING / ruling question / parked, and
   `grep -ril "needs ruling\|ruling question\|needs a ruling" internal/issue`. One line per item: issue
   file, the question in a sentence, the repro path if the file names one.

2. **Load the direction, not the code.** Read the design memories the index lists under "Language design
   rulings" (auto-memory `MEMORY.md`) and the ruling digests at the bottom of `internal/fix-issue-lessons.md`
   (sections named "Rulings <date>"). Grep both for each item's tag (q08, T418, ...) and for the concept
   (int -> double, extern ABI, drained state). The design north star: borrow by default, compiler infers
   copy/move, silent double free or leak never acceptable, C/C++ meaning where CFlat has no safety reason to
   differ, "refuse first, refine later", a frozen accept-set changes only by deliberate ruling. CFlat is a
   flattening of C, C++ and C#: when the parents agree, follow them. Read the repro `.cb` only when the
   question depends on a shape the issue text does not spell out; never open compiler sources for this.

3. **Split the list.** An item is OBVIOUS when one earlier ruling or design line decides it (cite it), or the
   two parents agree and no safety line cuts the other way. An item is OPEN when it is an ABI / layout /
   runtime-representation choice, when it re-raises a line the maintainer settled before (say so), when
   it changes the picks of compiling programs, or when the candidates differ only by taste. When unsure,
   it is open - a wrong self-ruling costs a fix round, a question costs one click.

4. **Rule the obvious ones.** In the issue file, at the exact spot that says NEEDS RULING (or at the top
   of the section), write one paragraph:
   `RULED <date> (main session, from precedent; maintainer may veto): <decision>. <precedent cited>.
   <what changes: which leg becomes err_, which spelling is the escape, which worktree is retargeted>.`
   Keep the original question text below it. Do not delete the file, even when the ruling closes the
   item as working-as-intended - the fix run deletes it.

5. **Ask the open ones.** One `AskUserQuestion` call, at most four questions. Each question: the shape in
   one or two sentences (what today does, what clang/C# does), then 2-3 options, the recommended one first
   and marked "(Recommended)", each option saying what it costs (accepted wobble, ABI change, legs that
   flip). The maintainer may answer with a clarifying question instead of an option: answer it in the
   next call's question text (one paragraph, concrete) and ask again with the same options. Do not
   restate options they already rejected. When they probe an option you dismissed, re-check the premise
   (2026-10-29: the "zero = drained" option was dismissed as rejected-before and turned out to be the
   maintainer's pick once the by-value-vs-pointer distinction was made explicit).

6. **Record the answers.** Same form in the issue file, marked `(maintainer)`:
   `RULED <date> (maintainer): <decision>. <what it supersedes, if anything>. <what changes>.`
   Then:
   - `Queue.md`: the row's note loses "NEED RULING" and gains "RULED <date>: <five words>"; a parked
     worktree the ruling retargets is named in the row.
   - A ruling that supersedes an auto-memory line: append a dated "Superseded" paragraph to that memory
     file and fix its `MEMORY.md` hook. Never write the ruling itself into memory.
   - Resume file, if one is live: drop the item from its "Rulings needed" list.

7. **Report.** Closing message lists the self-ruled items with their precedent in one line each and the
   maintainer-ruled items in one line each, then one sentence on anything the ruling makes unsafe or
   surprising (an accepted wobble, a count that changes). The wrap-up question is whether the self-ruled
   lines stand ("All stand" / "Veto some").

## Writing rules

- ASCII only; dates absolute; cite precedent by its tag or commit (q08 680d9e8a, T478, the 2026-09-24
  &ref() gate), never "an earlier ruling".
- One paragraph per ruling, no bullets inside it, <= 6 lines at the file's width.
- A ruling names the TEST CONSEQUENCE (which pinned leg flips to err_, which err_ leg becomes a value leg)
  so the fix brief can be written from the issue file alone.
- Never rule a `p4/` surface item (spelling of new syntax) yourself; those are always questions.
