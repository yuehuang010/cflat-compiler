# Lessons from the fix-issue rounds

Durable, cross-cutting lessons accumulated while working `internal/issue/` entries through
2026-07-28..30 (the `as`/`is` family, interface boxing, the return dangle, generic-interface
registration, and the four layers of the generic namespace key space).

These are here rather than in `internal/issue/` because they outlive any single issue. An issue
file is deleted when its bug is fixed; a lesson that changed an outcome more than once should
not be deleted with it. Every note below cost at least one confirmed defect to learn, and most
of them cost the same defect twice.

`internal/issue/interface-issue-queue.md` holds the live index and the landed design records for
the work these came out of. `internal/testing-notes.md` holds the mechanics of the suites.

---

## On reviews

- **A review round found a confirmed defect in nearly every round of this work. Never skip
  them.** Reviews repeatedly caught the fix agent's REASONING while its code was fine.
- **The generic-interface work took SIX rounds and produced SIX confirmed defect sets, every one
  found while `./test.sh` was GREEN** (522, 526, 530) - a SIGSEGV, a silent wrong-value
  miscompile, a stdlib-breaking false rejection, two dead checks. Layers 3 and 4 repeated it.
  **Read that correctly: not one of those defects was a leg asserting something too weak.** The
  suite asserts VALUES, not "it compiled", and it caught everything in its scope. Every miss was
  a shape with no discriminating input - no leg at all, or a leg whose only spelling could not
  reach the path. So the conclusion is not "distrust green"; it is that green answers only for
  the inputs someone chose to write, and that choice is made before the fix (see "On tests").
- **Scope each review round to what the last round CHANGED, and say what not to re-verify.**
  Rounds that re-covered settled ground burned budget; the rounds that found the worst defects
  (laundering, dead backstops) were the narrowly-scoped ones.
- **A "safe with listed fixes" verdict is not "clean".** Read the list; twice the listed items
  included a factually false diagnostic and a test that could not reach its leg.
- **Run the review BEFORE the test merge, not beside it.** Launched in parallel to save
  wall-clock, the review found two silent wrong values *after* their tests had already been
  folded into `Test/`, and the merge agent deleted the scratch directory the review was still
  drawing witnesses from. The parallelism saved less than it looked like it would.
- **Build a PRE binary early and keep it until the very end.** A detached worktree at HEAD plus
  `cmake_build.sh release` is one command, and it converts "the agent says this is a tightening"
  into a verified before/after. It caught a report that framed a REGRESSION as a tightening, and
  separately caught a tightening whose claimed witness did not compile on PRE at all - the claim
  was true, the evidence was not.
- **A stalled or failed review agent is not a clean review.** One round's reviewer died mid-run
  with no report; treating the silence as "nothing found" would have shipped an unverified cache
  round-trip.
- Point reviewers at the TRUE master binary (`x64/Release/cflat`); make them rebuild BOTH sides
  and verify the master binary's identity themselves.
- **Verify the PROOF, not just the answer.** One proof drove both binaries with `--check`, was
  vacuous, and still reported the right conclusion. Demand real `-o` codegen. (Correction:
  `--check` DOES reach the zero-implementor rebox diagnostic.)
- Make reviewers state how they validated their own harness is non-vacuous. One ran its
  classifier against known-differing files in both directions first, and caught that 2 of its 3
  real diffs were HIDDEN because `expect_error` output looks like a diagnostic on both sides.
- Never let the agent that wrote a fix be the only one to hunt for its consequences. Rounds 6
  and 7 of the `if const` attempt each INTRODUCED the next defect while fixing the previous one.

## On changing approach vs. patching

- **When site enumeration misses twice, change the method - do not add two more sites.** The
  generic-interface fix cycled reject-at-end-of-compile -> reject-at-site -> delete-the-check
  before landing on **record-then-resolve**: record `{name, file, line, col, role}` where the
  facts are local, resolve once where the tables are complete. Recording cannot reject, so a
  MISSED site degrades to "no diagnostic" instead of to a false rejection.
- **Defer the decision to where the facts are complete, but capture location and role where you
  have them.** Two independent fixes (`2bcc5a0`, `09f1d56`) converged on this from opposite
  directions.
- **Check whether your precondition is TRANSIENT before rejecting on it.** "In
  `genericInterfaceInstances`, not in `interfaceTable`" reads like a bug state and is the normal
  state during monomorphization (`LLVMBackend.h:16301`). Deferring turned three legitimate
  shapes from rejected into working - the check was not merely mis-worded.
- **When you widen a ROUTING predicate, every VALIDATION predicate keyed on the narrow one
  becomes a hole.** `GetType`'s `isInterface` was widened; `IsInterfaceType` was not; an
  early-out then laundered a 1-slot vtable into an 8-slot interface. A 68-site sweep was needed.
- **Do not reuse a predicate across a change of QUESTION.** The return-dangle pass reused
  `CallIsPointerOpaqueIntrinsic` for its neutral set; that helper also admits `llvm.mem*`, sound
  for ITS question (a pointer value's escape) and wrong for this one, where a memcpy into the
  slot is a real write. The codebase already knew - the sibling `AllocaIsLoadStoreOnly` comments
  exactly this.
- **A deleted safety check needs its harm argument tested, not reasoned.** "A fat pointer with
  no vtable must crash at the first method call" was true of the CALL path and false as a
  general claim - the unrouted type is a PIPE you assign through. One experiment settled what
  three paragraphs of reasoning could not.
- **When an agent cites a justification, check it still holds AFTER the change it is
  justifying.** One justification was true when written and false after its own edit.
  Over-broad candidate sets are SAFE for suppression and UNSAFE for blame.
- **A "this is dead code, so it needs no handling" justification is a deferred bug.** Make the
  agent prove the grammar constraint it depends on.

## On guard polarity

The recurring failure mode of this whole family, stated once:

- **Prove what you reject; accept what you cannot prove.** The primitive-array boxing guard
  proves three things before rejecting (registered interface target, builtin-primitive source
  element, provably pointer-shaped source) and accepts everything else. A 493-file sweep plus
  ~30 probes found no false rejection.
- **An allowlist has the opposite polarity and fails.** The closure-widening fix's intermediate
  version accepted only a named function, an `IsFunctionPointer` value, or null - and
  false-rejected a legal `io.lam(k > 0 ? a : b)`, because a `?:` join carries none of the three.
  It had to become "reject only what can be PROVEN to be data".
- **Reading a helper's `false` as a negative claim is how false rejections start.**
  `IsOwningValue` answers only a `LoadInst`; treating its `false` as "not owned" produced one.
- **The polarity can invert when you move a check.** Boxing `?:` arms early skipped the
  whole-expression owned-return check entirely - which cured a false rejection and equally
  removed the check for arms that own nothing, turning a rejection into a compiling double free.
  The check had to move INTO the per-arm walk, with the opposite polarity from the outer one.
- **A guard placed after an implements check has more copies of that early-out.** Go find them:
  the fourth boxing site was missed on the first pass for exactly this reason.

## On the code

- **`LogError` THROWS - treat it as a control-flow edge.** Three bugs traced to state or an IR
  bracket left open on the unwind path. It also never returns on the CLI path (`exit(1)`), so
  code after it is dead - one "reports every offender" loop ran a single body and its dedupe set
  and RAII restore were dead code. Brief every fix agent on both halves.
- **State that steers name resolution must not survive a reset.** `currentNamespace_` steers the
  generic key space; `LogError` unwinding past a hand-rolled save/restore left it stale, causing
  FALSE REJECTIONS in every later file of a batched `--check`. Clear it in `ResetForReanalysis`
  and make every save/restore RAII. `test.sh` runs one file per process and **cannot express
  this class of regression**; `test.bat` and LSP re-analysis are the batching consumers.
- **Struct nesting and namespace nesting share ONE dotted key space.** A template in
  `namespace Outer` and one nested in `struct Outer` are both keyed `Outer.Box`. Recovering a
  declaring scope with `rfind('.')` on the key is therefore always wrong. This was the single
  most expensive mistake in the generics family - it shipped two silent wrong values. Record the
  scope in a parallel map at registration; never derive it from a key string.
- **A name must be RESOLVED once, where the scope that gives it meaning is still current, and
  the resolved result RECORDED.** Never re-derive a declaring scope from a key, and never
  re-resolve a spelling downstream. Learned four separate times in one feature.
- **Before tightening a name-only discriminator, sweep `core/` and `example/` first.** A
  name-only rejection once turned `int C()` in a lock group - which master compiles and runs
  correctly - into a hard error with a factually false message, and **the suite could not see
  it: no in-repo `.cb` used the construct.**
- **`--init` is load-bearing.** Any new field on `TypeAndValue` / `StructData` /
  `AnnotationValue` that an analysis reads, and any key or mangled-name change, MUST move the
  cache round-trip in `LLVMBackend.cpp` in the SAME change, or it is silently dropped on a warm
  cache and `expect_error` tests stop firing. The cache is a named-key JSON map, not a
  positional record, so an absent field is an unambiguous absence and cannot desync later ones.

## On tests

- **Assert VALUES, never "it compiled".** Every leg in `Test/` gives the wrong answer a
  different observable value from the right one (`Test("gf_collision_ns", Gf1NS.f(), 11)`), and
  the ambiguity legs assert both halves together so a collapse onto one instantiation cannot
  hide. This is why a green run is worth trusting for what it covers: the failure family here
  includes binding to the wrong template, which links cleanly and would pass any compile-only
  check. Keep this property when adding legs.
- **One spelling tests one code path - so enumerate the axes BEFORE writing the legs.** This,
  not assertion strength, is where the misses came from. Two axes were each missed twice: the
  **spelling axis** (bare vs qualified vs aliased vs inferred use of the same name) and the
  **collision axis** (the same name declared in two scopes, plus its unique-name twin - the twin
  is the only form that proves the bare spelling reaches its OWN key rather than being absorbed
  by a same-named global). Layer 4's first cut had value-correct legs for every shape and still
  could not see the bug, because all of them used the colliding spelling.
- **A test can PASS while unable to reach the leg it claims.** Two negative tests did - one
  pinned a shape caught by an EARLIER record site, the other passed only because a declaration
  and a conversion shared a line number. Both were caught by review, never by the suite. When a
  test pins a diagnostic several sites can emit, pin the ROLE-specific wording and prove which
  site fired. **Mutation-test it: delete the record, the test must fail.**
- **Build the test corpus BEFORE the fix**, recording each leg's verbatim pre-fix behaviour. A
  leg that passes on both the before and after binaries is testing nothing. This is the one
  process step that has worked every time.
- **Retire the corpus when its legs are merged.** A tracked corpus outside every test glob is
  run by nothing, so the next diagnostic or mangled-name change breaks it silently and someone
  spends an afternoon on a stale red file that was never authoritative. Move the verbatim
  pre-fix tables into the design record and delete the corpus with the issue.
- **Verify a negative test is non-vacuous against the RIGHT baseline.** A test can be vacuous vs
  master yet still be a real tripwire for a defect introduced mid-work. Record which binary it
  discriminates against rather than labelling it vacuous.
- **A test that pins a PATH in `expect_error` breaks on the other platform.**
  `ShortenDefSiteForDisplay` returns native separators, deliberately. Pin the basename +
  (line,col) TAIL only. `expect_error` is a plain `.find()` substring check
  (`LLVMBackend.h:1201`).
- **A deferred (end-of-compile) diagnostic cannot be caught by a SCOPED `expect_error` block.**
  The block closes first and prints `FAIL: expected error ... did not occur`, then `exit(1)`s
  BEFORE the real diagnostic - so the stated reason is the opposite of the truth. Use the bare
  file-scope form for anything deferred, and say so in the file.
- Bugs needing 2+ `expect_error` legs in ONE function are a recurring blind spot.
- **Docs must not sell an unreachable guard as the safety story.** Apply such a correction to
  the COMMIT MESSAGE too, since that outlives the doc.

## On agent reports

- **Agents have reported work that did not exist**: one returned a status update as if it had
  implemented the feature (worktree had 0 commits, 0 modified files); one claimed a review round
  "found the correctness core clean" while that review was still RUNNING. **After every agent
  report, check `git rev-list --count master..HEAD`, `git log`, `git status --porcelain`, and
  the diff yourself before believing any of it.**
- An agent that delegates can orphan a child that keeps writing to the worktree with no one
  watching. Detect that before spawning a replacement, or two writers corrupt the same files.
- **Counterexample worth trusting**: an agent reporting a bug ITS OWN tests caught is doing real
  work. Fabricated reports do not contain self-inflicted findings.
- When an agent proposes diverging from the issue file's fix direction, treat it as a RISK.
- Tell agents to use repo-root `scratch/` and never run `git stash`.
- **Before declaring an agent has flailed, check for a LIVE PROCESS** (`ps` for the worktree
  path), not just `git log` / `git status`. An agent resumed mid-turn can look idle at the exact
  moment you sample it. On 2026-07-31 a resume was misread as a flail on correct evidence (HEAD
  unmoved, tree clean, defect still live) and an opus escalation was spawned into the same
  worktree; the two writers collided. The escalated agent detected it and stopped, which is the
  only reason nothing was clobbered.
- **When an agent overrules your instruction and explains why, check whether it is right** - it
  often is. Twice on 2026-07-31 an agent rejected a key I suggested (`ElementOwningUnique`,
  which is only ever set for a case that could never match) and corrected my arithmetic about
  test counts. Both corrections were right. An agent that pushes back with a reason is
  displaying exactly the behaviour you want.

## On concluding something is unused or unsupported

- **A repo-wide grep proves the SPELLING is unused. It proves nothing about whether the
  CAPABILITY is real.** On 2026-07-31 `function<T>*` was rejected wholesale on the reasoning
  that master's success looked like a constant-folding coincidence and that no file in `Test/`,
  `example/` or `core/` used it. Both premises were checked; the conclusion was still wrong -
  the out-parameter had genuine store-through IR (`store ptr %storemerge, ptr %slot`), and the
  rejection removed a working language feature. It was also trivially bypassed by a type alias,
  so it was not even self-consistent.
- **Read the IR before calling a behaviour accidental.** "It only worked by luck" is a strong
  claim about the compiler's semantics and needs the same evidence as any other. If the emitted
  code does the right thing for the right reason, the capability is real.
- **A fix can be HALF DONE along a spelling axis you did not enumerate.** The same
  `break`-without-dims bug lived in both the direct `function<T>` branch and the function-type
  ALIAS branch. Two rounds fixed only the direct one while the issue file was already staged for
  deletion - the P1 would have been "closed" while still reproducing under `using Cb = ...`.
  Before deleting an issue file, re-run the repro through every spelling that reaches the same
  code path: alias, generic argument, namespace-qualified, nested.
- **When a widely-read type flag changes, audit every guard that READS it.** Setting `Pointer`
  on the function-pointer parser branch was correct and fixed real bugs - and it silently
  disarmed the `unique`-field shape guard (`!f.Pointer || f.ElemPointer`), so a
  `unique Lambda<T>*` field started compiling and freeing a CODE address. Found only by the
  third review. The blast radius of a type-flag change is every predicate mentioning that flag,
  not the feature you were working on.

## On issue files and severities

- **Probe an issue before scoping work from it, even a carefully written one.** Consolidating
  three files took under an hour of probes and corrected all three: one claimed a severity its
  repro does not support, one asserted a shape works that does not, and a queue note claimed two
  issues shared a fix that they do not. A filed root cause is a hypothesis with a citation, not
  a measurement.
- **A severity recorded from a repro DIRECTION is unverified.** Both "silent miscompile" claims
  that were never actually run turned out to be false rejections instead.
- **Verify crash claims with `-o` before believing them.** One "compiler crash, zero output" was
  the PROGRAM segfaulting: `--run` JITs in-process, so the two are indistinguishable.
- **Issues filed while looking at feature X tend to be described as X bugs.** Three separate
  files called the generic key-space bug an interface problem; it affects every generic template
  kind. Probe the neighbouring kinds before fixing - it is cheap and it sets the real scope.
- **Consolidate on the shared ROOT, not the shared symptom, and say what you did NOT merge.**
- Issue files can be wrong: one repro did not reproduce as written, another described a fault
  milder than reality. Have the fix agent verify the repro FIRST and report what it saw.

## On the differential corpus sweep

- **The strongest available evidence for "did any behaviour change?" is a whole-corpus A/B, not
  hand-written probes.** Build the parent commit in a separate scratch worktree, run `--check`
  (or compile+run where values matter) over EVERY `.cb` in `Test/` and `example/` with both
  binaries, and diff. One review did this across 417 files and found exactly two differences,
  both the intended new test legs - which settled a false-rejection question that no amount of
  targeted probing could have closed.
- Reach for it whenever a change touches something every program flows through: a guard
  predicate, overload resolution, symbol mangling, type-flag semantics. Targeted probes only
  prove the shapes you thought of, and the dangerous shapes are the ones you did not.
- Tell the reviewer to report the scratch worktree path so it can be removed afterward; a
  forgotten one shows up in `git worktree list` later and reads like an abandoned fix branch.

## On concurrent agents sharing a worktree

- **Concurrent agents WILL collide on scratch filenames.** A reviewer and a fix agent both wrote
  `scratch/keep.cb` and destroyed each other's evidence; the round-1 must-keep-working results
  had to be regenerated from scratch. Assign every agent a unique scratch prefix (`r2_`, `rev_`)
  in its prompt.
- **A delegating agent's "no live children" notification does NOT mean nothing is running.** One
  fix agent spawned a sub-agent and returned immediately; the task notification fired, the branch
  had zero commits, and the tree was clean - every git-visible signal said "flailed, escalate."
  A build was in fact running in that worktree the whole time. `lsof -c ninja` / `ps` identified
  which worktree owned it. **Check for a live process before escalating into an occupied
  worktree** - escalating blind was already a near-clobber once before.
- Escalation and cleanup both look at git state; git state alone cannot distinguish "not started"
  from "in progress."

## On deciding whether a deferral was correct

- **A deferred item is often two sub-cases with different answers.** A fix agent deferred a
  field-to-field leg saying it needed the SOURCE predicate widened. Testing the plain spelling
  showed it already diagnosed, which looked like proof the reason was wrong - so the deferral got
  called wrong. It was not. The leg was MIXED (plain source into generic destination - closable
  by re-keying the destination alone) plus FULLY GENERIC (both sides substituted - genuinely
  needs the source predicate, because the source gate short-circuits independently). The agent
  was correct-but-incomplete; the correction overcorrected.
- Before overruling a deferral, enumerate the sub-cases and test each. "The plain spelling works"
  proves the machinery exists, not that every spelling reaches it.
- Record the overcorrection in the issue file too. A confident wrong correction in the history is
  worse than the original gap, because the next reader trusts it.

## On calling something a regression

- **Permute the input before declaring master correct.** A branch turned a program that printed
  `106` on master into one printing `206` - an apparently clear regression. But master printed
  `206` for the same program with the two overload declarations SWAPPED: it was never resolving
  by shape, just picking the first-registered symbol. The regression was real, the "previously
  correct" framing was not, and the fix ended up strictly better than master rather than a
  restoration.
- For anything order-, arity-, or declaration-sensitive, run the permuted form on the OLD binary
  before writing "previously correct" into a review or a report.

## On proving a test leg reaches the arm it names

- **A test leg must be proven to reach the arm it names, not just to fail on master.** A
  regression leg was written to pin a specific overload-resolution arm - the multi-candidate
  "take the first slot" fallback in `ResolveInterfaceMethodSlot`. It declared two overloads,
  `lam(function<int(int)>)` and `lam(int, int)`. Those have DIFFERENT arities, so for a
  one-argument call the arity filter left a single candidate and the LONE-SLOT arm ran instead -
  the arm an earlier leg in the same file already covered. The leg did fail on the pre-fix binary,
  so the usual non-vacuity check ("fails on master, passes on the branch") PASSED and hid the
  problem: it failed for the wrong reason. Deleting the gate the leg was written to protect would
  have left the suite green. The correct construction needed two candidates of the SAME arity
  (`lam(function<int(int)>)` and `lam(double)`).
- Why it matters: "fails on master" proves the leg is not vacuous; it does NOT prove the leg tests
  what its comment claims. When a leg is written to pin a SPECIFIC branch of a resolver, the check
  has to be that the branch is reached - e.g. by confirming the discriminator that selects it
  (here, candidate count after the arity filter), or by temporarily reasoning through the filter by
  hand. State the discriminator in the test's comment so the next person can see the leg's
  validity condition.

## On issue files referenced by a commit before it lands

- **An issue file referenced by a commit message must be tracked before the commit.** A commit
  deleted two P1 issue files it had fixed, and its message pointed at a follow-up issue file as the
  record of a deliberate deferral. That follow-up file existed on disk but had never been `git
  add`ed - it was untracked in the main checkout. The commit would have removed two tracked issues
  while the deferral it promised to record existed nowhere in history. A reviewer caught it with
  `git ls-files <path>`.
- Why it matters: `internal/issue/` files are routinely created untracked and stay that way for a
  while, so "I wrote that file" and "that file is in git" are different claims. Any issue path
  named in a commit message is a promise about HISTORY, not about the working tree. Before
  committing, run `git ls-files` on every issue path the message mentions, and add the referenced
  files to the same commit.
