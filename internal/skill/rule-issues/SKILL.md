---
name: rule-issues
description: Decide the "needs ruling" items parked in internal/issue/ - the main session works every item to a decision (precedent, parent languages, a measured spike), makes the precedent-backed ones fix-ready itself, and puts the judgment calls to the maintainer as one-click questions with prior art attached. Every ruling is recorded in the issue file. Use when the user says "answer the rulings", "rule the open questions", "decide the rulings", "work through the ruling list", "practice run", or a resume file / Queue.md carries a NEEDS RULING list.
---

# Rule the open issues

A ruling is a decision on language surface or semantics that an issue needs before it can be fixed. The
maintainer is the bottleneck, so every item leaves this run DECIDED: either ruled, or reduced to a question
the maintainer answers in one click with nothing left to look up. "Unsure" is a reason to spike, never a
reason to park. Every ruling lands in the issue file, nothing else: `internal/fix-issue-lessons.md` is for
lessons, auto-memory never holds one (the repo is the record). Nothing is committed (do-not-commit rule).

Measured 2026-11-18 (blind practice run, 11 answered): rulings resting on a cited precedent matched the
maintainer 4 of 5; judgment calls (safety, reversible default, parents disagreeing) matched 2 of 5. That
split is why step 4 separates the two. Re-measure with the calibration run below before widening it.

## Steps

1. **Collect the list - wider than the parked table.** Sources, all of them:
   - the live resume file (`scratch/resume_*.md`, newest): its "Rulings" section AND log lines saying
     `Ruling candidate`, `Ruling-list item`, `maintainer restate`, `Minor confirm`, `veto check`,
     `held for maintainer`, `Never asked`, `STOP (... feature`;
   - `internal/issue/Queue.md`: the "Parked - needs a maintainer ruling" table and any row whose note says
     NEED(S) RULING, `design call`, `open:`, `veto`, `ruling question`;
   - `grep -rli "needs ruling\|needs a.*ruling\|ruling needed\|ruling owed\|ruling question\|design call\|maintainer may veto\|open question\|maintainer decision" internal/issue`;
   - earlier self-rulings still waiting on a veto (`RULED ... maintainer may veto` with no fix landed).
   One line per item: issue file + item number, the question in a sentence, the repro if the file has one.
   A question that lives only in the resume log, or has no issue file at all, is written into the closest
   issue file first, repro folded in (issue files are standalone).

2. **Load the direction, not the code.** Read the design memories the index lists under "Language design
   rulings" (auto-memory `MEMORY.md`) and the ruling digests at the bottom of `internal/fix-issue-lessons.md`
   (sections named "Rulings <date>"). Grep both, and `RULED` lines across `internal/issue`, for each item's
   tag (q08, T418, ...) and concept (int -> double, extern ABI, drained state). Never open compiler sources
   to decide; a question only source reading can answer is a bug investigation - say so in the issue file
   and drop it from the list.

3. **Work each item to a pick.** Go down the list; the first line that settles it is the RUNG, named in
   the ruling. The order is how the maintainer rules, learned from the misses of 2026-11-18.
   1. PRECEDENT. One earlier `(maintainer)` ruling or design line decides it. Cite the tag or date. Check
      SCOPE lines before anything else, because they cancel a tempting refusal:
      - memory safety is OWNERSHIP at function boundaries; a borrow the user invalidates inside a function
        (mutating a container under a foreach, a stale raw pointer) is C++ behaviour, not a compile error;
      - CFlat exposes no const / volatile: a C++ cv refusal that needs one is not reproduced;
      - text that is not cflat's own is prefixed with its real source (`clang: `, `linker: `).
      A self-ruling is never precedent for another self-ruling - go to the maintainer ruling under it.
   2. PARENT MEANING. CFlat is a flattening of C, C++ and C#. If the spelling ALREADY PARSES and a parent
      gives it a meaning, the ruling is IMPLEMENT THAT MEANING, however large the build (`static` field ->
      shared storage; `int[n] a` with a runtime n -> a runtime-sized array). "Refuse until designed" is
      not a ruling the maintainer makes for an existing spelling; an interim refusal is offered only as a
      second option. Where C and C++ DISAGREE, C++ wins (cast nulls at a function slot refused, the C
      NULL-body argument lost). C++ interop shapes: clang's verdict and counts, obtained from clang itself
      rather than by re-implementing a clang rule.
   3. COMPILER SAFETY. The compiler's own act is wrong: an inserted release that double frees, an owned
      object it leaks, a miscompile. Then the safe behaviour wins. Not this rung: anything the user does
      to themselves (rung 1 scope lines). And non-null is not owned - a "release if non-null" answer needs
      a stated owner first.
   4. SPIKE. The candidates differ on a fact, so measure before choosing (never offer a menu on an
      unmeasured premise). One batched probe script under `scratch/rule_<date>/`: what master does for
      each shape (exit code checked, never piped through grep; the sizeof / count / value, not only
      "compiles"), what clang does for the C++ twin, how many `Test/` legs each candidate flips, and for a
      perf / memory question the number itself. Read-only on the compiler: no source edits, no suite
      runs, no torch. Fold the probe and its output into the issue file; delete the scratch folder at the end.
   5. DEFAULT. Still tied: the option that keeps the most programs compiling with the parent meaning,
      then the one with no new syntax, no ABI / layout change. Not "refuse first": that line orders the
      steps of a FIX (ship the refusal, widen later), it does not pick between meanings.
   A DESIGN item (owner bit on argument / field / global slots) has no candidate list yet. Do not put it
   on a choice card. Write the mechanism in plain words in the issue file - what each slot does today,
   the shape that goes wrong, why the obvious answer fails, 2-3 candidate models with what each costs -
   and raise it in the closing message as prose. It becomes a card only once the maintainer has engaged.

4. **Sort the picks: fix-ready or question.**
   - FIX-READY (self-ruled, no veto hold): the rung is PRECEDENT with a `(maintainer)` ruling cited that
     decides the same shape (not an analogy), and the pick does not turn a compiling program into an
     error unless that cited ruling already refuses a sibling. Write
     `RULED <date> (main session, PRECEDENT <tag>; maintainer may veto): <decision>. <rejected candidate,
     one clause>. <test consequence>.`
   - QUESTION: every other rung, every p4 surface item (spelling of NEW syntax or a library name -
     CLAUDE.md: never started without a maintainer ruling), anything that reverses or narrows a
     `(maintainer)` ruling, anything whose undo is not one commit (shipped cache / ABI / mangling format,
     feature deletion, root `vcpkg.json`, LLVM pin). Write `PROPOSED <date> (main session, <rung>): ...`
     in the issue file first, so the pick and its evidence survive an unanswered card.
   A pick that disagrees with an earlier recommendation in the resume log says so in its paragraph.

5. **Ask the questions.** `AskUserQuestion`, four per call, as many calls as the list needs, most
   queue-blocking first. Each question must be answerable with no other context - one in three needed a
   clarification round on 2026-11-18, each for one of these:
   - plain words, no internal names: not "owner bit" / "check 2" / "item 2" but what the program does;
   - the shape as code, what master does today (measured), and what C, C++ (clang), C# and Rust each do
     with it - always, the maintainer asks for it otherwise;
   - what is NOT in question (`nullptr` itself stays accepted; only the cast spellings are asked);
   - whether it is specific to one form (ternary only?) or general;
   - 2-3 options, each with its cost (legs that flip, build size, divergence kept).
   Normal run: the proposal first, marked "(Recommended)". The maintainer may answer with a question or a
   free-text direction instead of an option: answer it in the next call's question text (one paragraph,
   concrete) and ask again; do not restate options they rejected. "Take this outside the questions" means
   stop carding that item and treat it as a DESIGN item. A free-text answer is the ruling's direction even
   when it matches no option - record their words, then the consequence you derive, marked as derived.
   Run as a subagent (no `AskUserQuestion`): write the PROPOSED paragraphs and return the questions in the
   report; the main session asks.

6. **Record.** Maintainer answers: `RULED <date> (maintainer): <decision>. <rejected candidates, naming the
   main session's pick when it lost>. <what it supersedes>. <test consequence>.` replacing the PROPOSED
   line, original question text kept below. A confirmed self-ruling gains `CONFIRMED <date> (maintainer)`;
   a veto is `VETOED <date> (maintainer): <replacement>` above it. Then:
   - a ruling that implements a feature or needs a plan: write the plan stub under `internal/plan/`
     (status, ruling, where it stands, phases, open questions) and name it in the issue file;
   - `Queue.md`: the row leaves the "Parked" table; its family row gains `RULED <date>: <five words>` and
     `buildable` / `plan first` / `closed`; a retargeted parked worktree is named;
   - resume file, if live: the item moves from the rulings-needed list to a ruled list with its issue path;
   - a ruling that supersedes an auto-memory line: append a dated "Superseded" paragraph pointing at the
     issue file and fix the `MEMORY.md` hook. Never write the ruling itself into memory.
   - Do not delete an issue file, even when the ruling closes it as by design - the fix or doc run does.

7. **Report.** Closing message, most consequential first: rulings that turn a compiling program into an
   error or change a runtime result (rung, legs that flip, issue path); the other rulings, one line each;
   what is still open and why; any DESIGN item as a short plain-words explanation ending in the question.
   The wrap-up question is whether the self-ruled lines stand ("All stand" / "Veto some").

## Calibration run (on request: "practice run", "see if the recommendation matches")

Same steps, with the picks hidden: write every pick and its rung to `scratch/rule_<date>/picks.md`
BEFORE asking; ask with options unmarked and the pick not always first; afterwards tally MATCH / MISS /
PARTIAL per rung. For each miss write one line in step 3 naming the principle that would have produced the
maintainer's answer (not the answer itself), and update the measured line at the top. Widen FIX-READY to
another rung only after it matches at least 4 of 5 in a run. Record the answers as real rulings only when
the maintainer says to.

## Writing rules

- ASCII only; dates absolute; cite precedent by its tag or commit (q08 680d9e8a, T478, the 2026-09-24
  &ref() gate), never "an earlier ruling".
- One paragraph per ruling, no bullets inside it, <= 6 lines at the file's width (design write-ups <= 12).
- A ruling names the TEST CONSEQUENCE (which pinned leg flips to err_, which err_ leg becomes a value leg,
  the value the repro prints after the fix) so the fix brief can be written from the issue file alone.
- A ruling names what it did NOT decide (fields and globals with a runtime bound, a zero bound) so the fix
  run does not invent it.
