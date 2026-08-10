# Lessons from the fix-issue rounds

Durable, cross-cutting lessons accumulated while working `internal/issue/` entries through
2026-07-28..30 (the `as`/`is` family, interface boxing, the return dangle, generic-interface
registration, and the four layers of the generic namespace key space).

These are here rather than in `internal/issue/` because they outlive any single issue. An issue
file is deleted when its bug is fixed; a lesson that changed an outcome more than once should
not be deleted with it. Every note below cost at least one confirmed defect to learn, and most
of them cost the same defect twice.

Active issues live one file per issue under `internal/issue/` (`p2/`, `p3/`, `ui/`). The old
`internal/issue/interface-issue-queue.md` index was retired on 2026-08-08; its durable lessons
were folded in below and its landed design records survive as the digest at the bottom of this
file. `internal/testing-notes.md` holds the mechanics of the suites.

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
- **Keep the PRE binary OUTSIDE the repo - `scratch/` is the fix agent's workspace, not the
  reviewer's.** Second occurrence 2026-08-03 (the first was a merge agent deleting the scratch
  directory a review was still drawing witnesses from): a PRE binary parked in `scratch/pre/` was
  deleted by the Stage 1 implementation agent tidying up, and had to be rebuilt mid-review. Build it
  in a detached worktree under `/tmp` and reference it by absolute path. Corollary: **verify the PRE
  binary's identity before quoting it** - the first sign anything was wrong was every probe
  returning exit 127, which is a missing binary, not a compiler verdict. A silent substitution
  would have been far worse than a loud one.
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

## On the reference you are matching

- **When the fix is "make X agree with Y", Y is an ORACLE and must be verified
  independently before you trust it.** On 2026-08-02 `S gs = {1,2};` at global scope silently
  zeroed while the identical local spelling was rejected, so the fix was specified as "make the
  global declarator agree with the local one". The agent copied the local path's gating
  condition (`initializer->initializerList()`), which carried the identical hole: a second brace
  spelling lives on `initDecl` directly, so `S gs {1,2};` bypassed the new guard entirely and
  the filed bug survived its own fix. **Agreement with a broken reference is invisible to a
  strategy built on agreement** - every check available to that strategy said the two scopes now
  matched, and they did, including in the hole. The measurement that exposed it was running the
  ORACLE against itself: local `S ls = { a=1,b=2 }` gives `a=1 b=2`, local `S ls { a=1,b=2 }`
  gives `a=0 b=0`.
- Corollary for the axis list: a construct's SYNTAX spellings are a separate axis from the types
  and scopes it appears in. That enumeration covered struct/union/class, global/local,
  namespaced, `static`, `const`, containers, arrays and nesting, and never asked how the
  initializer itself could be written. The existing spelling-axis lesson is about NAME spellings
  (bare/qualified/aliased); this is its syntactic twin and was missed because the first pattern
  matched so readily.

## On claims of equivalence between two binaries

- **"Pre-existing" / "unchanged" / "not a regression" is a MEASUREMENT, and it must be taken in
  the exact spelling the claim is about.** Two such claims were wrong in a single 2026-08-02
  issue. A filed P1 said its repro was "identical on both the pre-change and post-change
  commits" using the bare spelling; measured, `S* p = {a=1}` really was identical (`0x1` both sides) while
  `S* p {a=1}` went from `ptr undef` to `inttoptr (i64 1 to ptr)` - changed by the very commit
  filing the issue. Separately a coverage cell reported as "now deep-copies exactly like the
  named-local spelling" in fact leaked 16 bytes the named-local form does not.
- Both errors have the same shape: equivalence INFERRED from a sibling spelling rather than
  measured per spelling. In an issue where two spellings behaved differently pre-fix - which was
  the whole finding - inference between them is exactly the step that cannot be taken.
- Cost: one round-trip each, after the fact, on work already believed finished. Requiring a
  measured pre/post pair with every equivalence claim is cheaper than any of them.

## On changing approach vs. patching

- **When site enumeration misses twice, change the method - do not add two more sites.** The
  generic-interface fix cycled reject-at-end-of-compile -> reject-at-site -> delete-the-check
  before landing on **record-then-resolve**: record `{name, file, line, col, role}` where the
  facts are local, resolve once where the tables are complete. Recording cannot reject, so a
  MISSED site degrades to "no diagnostic" instead of to a false rejection.
- **Defer the decision to where the facts are complete, but capture location and role where you
  have them.** Two independent fixes (the return-dangle record and the generic-interface
  registration record) converged on this from opposite directions.
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
- **Build the accept-set BEFORE the guard, not after.** Enumerate and freeze as value legs the
  programs in the same neighbourhood that master compiles and runs correctly, then write the
  rejection. Reviewed on 2026-08-02 across five issues: false rejections are the single largest
  consumer of rounds in this repo. Two issues were parked having landed nothing after three rounds
  each (the funcptr-signature round, four false rejections; the delete-borrowed-box round, five), and a third
  spent a full fix+review round adding a rejection site and taking it back out. The accept-set gets
  built either way - the reviewer cannot judge a guard without it. Building it first costs a
  fraction of building it as review findings.
- **Do not add a site to a rejection because a probe printed a strange number.** Establish that the
  site is broken from the `--no-opt` IR. On 2026-08-02 a named-argument site was rejected on a probe
  reading `(int)(i64)p`; the number was a truncated stack address of a correctly materialized temp,
  and that site had never had the bug. Its three genuinely-broken siblings returned the packed field
  bytes (`0x1`, `0x200000001`) - an address versus field bytes is the whole distinction, and it is
  invisible in the decimal value. The rejection's own message was false at that site.

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
- **Assert the RESOURCE, not only the value, when the change touches ownership or lifetime -
  and for EVERY owning type the change can reach.** On 2026-08-02 a unique-field fix wrote 23
  value legs that rigorously asserted destructor counts (`no_early_free`, `freed_once`,
  `source_nulled`) for the `Node*` it was reasoning about, and asserted values only for the
  `string` case. The values were correct - the leaked buffer was the OLD destination, not the
  one being read - so the leg passed while the commit leaked 16 bytes. Nothing in the suite said
  so; the evidence was `Test/test_move.cb` itself moving from 13 allocations / 256 bytes to
  14 / 272 under `leaks --atExit`. The discipline was present and applied to one axis; the
  defect landed in the neighbouring one.
- **A leg that cannot fail is worse than no leg, because it reads as coverage.** The same file
  carried `Test("uae_string_elem_copy_survived_teardown", 1, 1)` - literally `1 == 1`. Sweep new
  legs for this before reporting: any leg whose expression cannot produce a value other than the
  expected one, and any leg that would still pass with the fix reverted.
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
- **A stale crash SIGNATURE does not mean a healthy area - re-measure the area, not the
  signature.** `llvm-cannot-select-sign-extend-on-const-array-index` was named for a fatal error
  that stopped reproducing on every one of 12 probes; re-running the axes it had never enumerated
  found four live failures in the same area, one of them a compiler SIGSEGV worse than the crash
  the file was named for. Renaming it to describe the measured behaviour
  ([[fixed-array-storage-guards-miss-four-axes]]) was worth the churn of updating three
  references: a slug naming a dead symptom sends the next reader looking for the wrong thing.
- **"The shape that fed it is rejected now, so there is no live repro" is a hypothesis about
  every OTHER shape.** It has been written twice in this repo and been wrong twice. Enumerate
  the axes - decl-init, assignment, compound assignment, global, join, parameter, return, field,
  element-of-multidim - and only then write it.

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
- **Hand the corpus over; do not build it twice.** The fix agent's probe corpus stays in `scratch/`
  until the merge, and the reviewer spot-checks it rather than reconstructing one. Measured over
  2026-08-02's rounds, fix agent and reviewer each spent roughly 130k tokens on probes, and the
  duplicated half never found anything: every real defect came from attacking an axis the fix
  agent's corpus did not cover. Re-measure a sample to confirm the report's pre/post pairs are
  real - that is cheap - and spend the rest of the budget on the gaps.
- **A zero-difference sweep rules out NOTHING when the corpus performs no crossing.** The sweep
  answers "did any file I already have change behaviour", never "is the new rule correct". On
  2026-08-02 a funcptr comparator swept 424 files with zero differences and was then shown to
  hard-error on three programs master runs correctly, each reachable in about three lines. No
  corpus file crossed a signedness or namespace boundary at a `function<>` argument, so the sweep
  structurally could not see it. When the change adds a REJECTION, the sweep is the weaker half of
  the evidence: the stronger half is a TARGETED must-still-work corpus that deliberately crosses
  every equivalence boundary the new rule could mistake for a difference. Build that first.
- **A macOS sweep never compiles the Windows-only core.** `core/com.cb` and `core/ui_native/*.cb`
  are not in the swept set on this host, and they are where some type spellings are dense. A green
  sweep here says nothing about them - reason about the affected spelling instead of counting files.
- **A sweep run with `--check` cannot see a codegen crash.** `--check` stops before the backend,
  so a program that segfaults the compiler under `-o` can pass `--check` with exit 0. Measured
  2026-08-02: `char[2][8] b = default; b[0] = "hello";` gives `--check` rc 0 and `-o` rc 139 with
  zero output. If the change touches anything that lowers, sweep with a real compile on at least
  a sample, or the sweep's green is only about the front end.
- **Do not pipe the compiler into `head` and then read `$?`** - you get `head`'s exit code, not
  the compiler's. Two "exit 0" readings in the 2026-08-02 array sweep were this mistake.
  Redirect to a file, then check `$?`.

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
- **The two `expect_error` forms have DIFFERENT multi-leg semantics; do not carry one file's rule
  over to the other.** The SCOPED-BLOCK form (`expect_error("...") { ... }`) supports many legs in
  one file: each block is armed and checked independently, so a later leg IS self-proving. The
  BARE-SEMICOLON form is the one that stops at the first error, because the expectation covers the
  rest of the enclosing scope. On 2026-08-02 the main session told two fix agents "one reject leg
  per file, a file exits at the first error"; both agents tested it rather than complying, and both
  disproved it for the scoped-block form by mutating each leg separately and watching the file flip
  to exit 1 every time. **The agents were right and the instruction was wrong.** The real rule is
  the mutation test itself: mutate each leg individually and confirm the file fails for that leg.
  That works for both forms and needs no assumption about file semantics.
- **Verify a leg fires the NEW guard, not a pre-existing one.** In an area that already rejects
  several neighbouring shapes, a leg can pass for a reason the diff did not create. One 2026-08-02
  matrix recorded three cells as "already rejected by a pre-existing guard" and one of them was in
  fact ACCEPTED and a live double free - recorded on the safe side without being run.

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

## On the baseline binary a severity claim is measured against

- **Pin the baseline to a binary you verified is the merge-base, in the configuration you name.**
  On 2026-08-02 the main session filed a P1 whose headline was "the compiler SIGSEGVs, exit 139,
  zero output" for `char[2][8] b = default; b[0] = "hello";`. A review re-measured it against a
  verified merge-base Release build: the compiler exits 0, links, and the program RUNS to exit 0
  printing garbage. It is a SILENT MISCOMPILE, not a crash. The 139 came from a stale binary, and
  a stale Debug binary in the same tree asserts with rc 134 - three different "baselines", three
  different severities, one program.
- The false claim had already been committed into a test-file comment, so the record outlived the
  measurement. Before writing an exit code into an issue file or a test comment, confirm the binary
  you measured with: check its mtime against the source, and confirm the commit it was built from.
- **Severity CATEGORY, not just the number, drives triage.** Crash / silent-wrong-value /
  hard-error are ranked differently by this queue, and a wrong category sends the next round after
  the wrong evidence. Here the corrected category was WORSE than the filed one, so the P1 survived
  on its merits - do not assume a correction always demotes.
- Corollary: the same construct can be a crash in one containment and a miscompile in another.
  `u.a[2] = 9` on a `union U { int[4] a; double d; }` really is a compiler SIGSEGV on the same
  master where the plain local miscompiles silently. "It crashes" and "it does not crash" were both
  true of neighbouring spellings, which is exactly why the repro must be quoted verbatim with its
  measurement.

## On a fix that MASKS a pre-existing defect

- **A new rejection can hide a crash rather than fix it; check what the guard is standing in front
  of.** The 2026-08-02 array-storage branch false-rejected every element write into a union array
  field. That same false rejection also suppressed a genuine pre-existing compiler SIGSEGV on
  `u.a[2] = 9`. Removing the false rejection is correct AND re-exposes the crash, so the fix round
  has to carry both. A guard that turns a crash into a hard error looks like progress in the suite
  and is not, if the programs it rejects are correct ones.
- **The mirror: UNBLOCKING compilation moves a program onto runtime paths a rejection was hiding,
  and those paths can be keyed on a spelling your fix just changed.** The generic-closure-element
  lowering fix made a
  lambda literal into a generic-substituted field compile for the first time (it had died in the
  module verifier), and the program promptly ran onto the `=` path's closure OWNERSHIP-transfer
  arm - which tested `TypeName == "__closure_fat_ptr"` and therefore skipped the encoded name. The
  env was freed while the field still pointed at it: wrong value, then SIGSEGV, i.e. a compile-time
  rejection turned into a miscompile. When a fix newly ADMITS a construct, enumerate the ownership,
  destructor and lifetime predicates that construct will now reach, and check each for a
  SPELLING-keyed test that should have been a REPRESENTATION-keyed one. A sibling site getting it
  right (here `EmitOneFieldInit` keyed on `val->getType() == GetClosureFatPtrType()`) is the tell.
- **An axis-at-a-time coverage matrix silently drops CROSSES.** The same round's matrix listed
  "capturing literal" on the operation axis and "fat element" on the type axis, and had no cell
  where both held - fat literals were probed non-capturing, capturing literals only against thin
  destinations where they are rejected before any ownership path runs. The miscompile lived exactly
  in the missing cross. Freezing an accept set per axis is not the same as covering the product;
  before declaring a matrix complete, name the crosses you are NOT taking and say why.

---

# Additions distilled from the retired issue queue (2026-08-08)

Everything below was extracted from `internal/issue/interface-issue-queue.md` when it was
retired. The lessons extend the sections above; the digest at the end is the permanent record
of the landed design records - every "do not retry" in it is a settled decision, measured and
in most cases already attempted and reverted once.

## On issue files and root causes (additions)

- **"The compiler has no way to know X" in an issue file is a claim about the CODEBASE, and it
  is often already false - probe whether the signal exists before designing one.** The
  temp-source unique field stores issue file said the temp-source provenance was missing;
  `MovableTempField` / `FromOwningTempField` already rode the exact spellings, and the only
  blocker was an unrelated gate. Two other records repeat it (the stack-or-global roots record:
  "globals have no `NamedVariable`" was false; the aliased-copy-at-declaration record: the filed
  direction would have false-rejected three correct programs). A filed fix DIRECTION is a
  hypothesis about the code, and it is wrong more often than the filed repro is.
- **Driving a queue's P1 COUNT to zero does not converge; scope the next campaign on the
  SEVERITY MIX.** Two campaigns each fixed six P1s and ended with the count unmoved, because an
  adversarial review in this area reliably splits out neighbouring defects. What converged was
  severity: closed items were silent double frees and zero-output crashes; the replacements were
  narrower and diagnosed. Ask "are there silent wrong values left", not "is the count zero".
- **An LLVM assert/fatal/verifier failure reachable from plain source is P1 whether or not it
  aborts** - a raw verifier dump with no `file(line,col):` prefix is "dies with no usable
  diagnostic", and a LOCATED diagnostic is the floor for such a row even when the underlying
  feature gap stays open.
- **The single largest source of entries in the retired queue was bookkeeping duplicated across
  sites, each copy carrying a different subset of the guards** (interface boxing at four sites,
  overload scoring as three hand-copied probe/replay pairs, generic name resolution with three
  disagreeing key conventions). And every fix in the boxing family was blocked on PLUMBING -
  getting the source `NamedVariable` to the guard - not on guard logic. **Look for the missing
  INPUT before writing a new check.**

## On where a gate has to be installed (repo inventories)

- **A value reaching a DESTINATION has at least nine distinct lowering paths in this codebase,
  and none of them share code.** The code-value-into-data-destination store fix needed nine gate
  sites, the closure-provenance assignment-leg fix
  seven, and each round found more by review, not by reasoning. The standing checklist: the `=`
  operator (`ParseAssignmentExpression` - alone covers local / field / nested field /
  through-pointer / array element / global), the declarator initializer, `return`,
  `EmitOneFieldInit` (brace field init - also the funnel for `new T{...}` and the `<Tag attr=>`
  sugar), `EmitPositionalFixedArrayInit`, `EmitArrayViewInferredInit`,
  `EmitGlobalFixedArrayInit`, `ParseFieldDefaultInitializer` (shared by FIVE default-ctor
  emitters; the union arm of `ParseStructDefinition` reads no field initializers at all), and
  `GenerateDefaultParamOverloads` (a parameter default - the call-site gate structurally cannot
  see it, because the wrapper rebuilds the forwarding `NamedVariable` with the DESTINATION's
  TypeName and launders provenance). `??=` used to be a tenth until the `??=`-shared-store-tail
  record routed it through the shared tail.
- **An ARGUMENT-position gate has four doors, and `CreateOverloadedFunctionCall` is only one.**
  `LLVMBackend::CallInterfaceMethod` (virtual dispatch) and `CreateIndirectCall` (a `function<>`
  value, thin and fat arms) lower their own argument lists and enter neither the scorer nor
  `ResolveInterfaceMethodSlot`; a monomorphized generic closure parameter is not
  `IsFunctionPointer` at all, so a gate keyed on that flag alone misses `list<function<>>::add`.
  the shape-mismatched-funcptr-argument record and the pointer-argument-by-value-parameter
  record each shipped a first cut covering only the direct path
  and each had a live SIGSEGV face found by review.
- **Count the registration sites empirically before applying the both-copies rule.**
  `ForwardRefScanner::ScanExternalDeclaration` has no `declaration()` dispatch arm, so a
  bodyless prototype never reaches the pre-pass and a mirrored guard there is dead code
  (the bodyless-prototype fixed-array-return record). The opposite also happened: the
  unresolved-generic-name-opaque-shell record's filed citation named
  one site, and a SECOND ungated copy lived in `ForwardRefScanner::ParseDeclarationSpecifiers` -
  gating the first left the headline repro compiling. Read the dispatch arms; assume neither
  "one site" nor "two copies".
- **An end-of-body deferred check runs only in the named-function path.** `RunNullDerefDataflow`,
  `RunInterfaceReturnDangleCheck`, `RunNullIfaceDispatchCheck` and
  `RunUniqueIfaceFieldStoreCheck` all share this: a flagged store inside a LAMBDA body is never
  settled. Record it as known residue rather than re-deriving it each round.

## On ledgers keyed by `llvm::Value` identity

- **Before keying a ledger on `llvm::Value*` identity, ask whether that value is a shared
  module-level constant.** A named function is ONE `llvm::Function` for every mention in a body,
  and `nullptr` is ONE `ConstantPointerNull` for the whole module. The code-value-evidence
  join-ledger record held its
  launder ledger for a whole function and one `void* v = (void*)ro;` laundered `ro` for every
  later gate in that function; the closure-widen-gate join record created and closed the mirror
  of the same hole
  when `(function<>)nullptr` made an unrelated join's null arm read as user-asserted code.
  Statement scope helps and is not sufficient - `RegisterDataValueCodeCast` had to STRUCTURALLY
  refuse `ConstantPointerNull` and `llvm::Function`. Contrast the temp-unique-field-escape record, where each
  temp-field read is a distinct `ExtractValue` and value identity is safe by construction; that
  difference is the thing to check, not the pattern to copy.
- **A statement-scoped ledger's retire point is `FlushOwnedTemps` at the block-item boundary**,
  which is also where an owning temp's destructor runs - an entry can never outlive the dangle
  it describes. Reuse that boundary rather than inventing a scope.

## On mirror gates and quantifiers

- **Two gates asking opposite questions need OPPOSITE quantifiers, and copying the sibling's
  polarity is a false-rejection generator.** `JoinCarriesCodeValue` (proving CODE) is correct as
  "ANY arm answers yes"; `JoinDeliversDataValue` (proving DATA) must be "EVERY arm proven, at
  least one proven, unproven means fail-open". The closure-widen-gate join record proved it by mutation: flipping
  every-arm to any-arm turned three mixed-arm legs into hard rejections of programs the merge
  base runs correctly.
- **ARM POSITION is its own axis, and `?:` / `??` arms are not symmetric.** A `?:` arm gets an
  explicit `FlushOwnedTempsSince` inside the arm block; the `??` path makes no such call, so its
  fallback-arm temps are never destructed. Measured in the temp-unique-field-escape record: `makeBox().t ?? nullptr`
  dangles (reject) while `p ?? makeBox().t` is never freed (accept - rejecting it would refuse a
  working program). The walk follows `Arms[0]` only; when the leak is fixed the exclusion must
  be deleted in the same change, with a leg pinning `dtors == 0` so it goes red at that moment.

## On diagnostics (additions)

- **A diagnostic's stated mechanism and its REMEDY are factual claims - compile the remedy
  before shipping the message, per destination spelling.** This cost a round at least six times:
  the temp-source unique-field-store fix shipped three messages whose remedies were invalid
  syntax, a closed loop, or an
  abort; the plain-`T*`-parameter escape-fact record inherited a remedy the compiler rejects;
  the code-value-into-data-destination store record had to drop the `(T*)` cast advice for `T[]` and `string` destinations
  because the advice sent the user into a second error; the shape-mismatched-funcptr-argument record hit the identical thing
  with `&` on a VIEW parameter. Freeze the working remedies as VALUE legs, not just as message
  text. A remedy named inside a generic body must be valid at EVERY instantiation
  (the empty-brace-split-by-target-type record names `= default` first for exactly this reason).
- **Never recover a user-facing name from a lowered LLVM artifact.** The array-view
  delete-guard record read a source
  name off an `llvm::GlobalVariable`, which carries LLVM's `.N` uniquifying suffix on collision
  with a runtime symbol - an ordinary `int[3] read;` was reported as `'read.1'`. Take identifier
  text from the AST. Same family: a message may not quote a mangled symbol unless the rendering
  is provably writable source - `Box__Box__i32` and `dictionary__string__int` are
  indistinguishable as strings, so `DisplayNameOfMangledType` returns the raw name with
  `writable = false` and the caller DROPS the advice clause rather than emit a half-demangled
  hybrid (repeated across the pointer-argument-by-value-parameter record, the closure-pointer
  generic-type-argument record, and the generic-closure-element lowering record).
- **A DEFERRED (end-of-compile) diagnostic must carry the FILE it was recorded in, not just
  line/col.** By resolve time `sourceFileName` is the MAIN file again, so
  the plain-`T*`-parameter escape-fact record's first cut blamed the importing file at a line belonging to a
  comment. Record `{file, line, col}` at the site; wrap the reporting scope in RAII, since
  `LogError` throws.
- **At a shared reject site, LEG ORDER decides which wording fires, and reordering is a
  behaviour change.** The temp's-`unique`-field-may-not-escape-the-statement record placed a new leg before the existing type-name leg
  and broke three legs of an existing error file. Place the new leg last and measure exactly
  which cells change wording.

## On the code (additions)

- **`""` / `0` on a newly-recorded field means NOT RECORDED, never "not a pointer" / "not a
  thing".** Established twice: `PointerDepth == 0` and `FuncPtrParam::ResolvedTypeKey == ""`.
  Only source-parse sites fill them; C interop, WinRT and synthesized signatures leave them
  empty and must keep binding. Any consumer reading the empty value as a negative claim is a
  false rejection waiting to happen.
- **When a call site reads a signature or parameter fact, it reads the one `ForwardRefScanner`
  registered - the SCANNER copy is the load-bearing one.** The last-two-funcptr-signature-items record filled only the
  three `MainListener` sites and the fix was a complete no-op for its own repro. A plan that
  says "codegen sites only" for anything the function table holds is wrong.
- **A new per-function field must join `BuilderState`'s snapshot in the same change, exactly as
  a new `TypeAndValue` field must join the `--init` round-trip.** `currentFunctionReturnTV` was
  added beside three snapshotted siblings and never joined them; a monomorphized instantiation
  mid-body then leaked its return shape into the enclosing function (`return 42;` came out as
  `ret ptr bitcast (i8 42 to ptr)`; a struct-value enclosing return was a compiler SIGSEGV).
  Take `createFunctionBlock`'s clear-list as the definition of "per-function state" and check
  each entry against `BuilderState`. That audit found a second, still-open instance:
  `aliasDomain_` / `aliasScopes_` / `viewScopeByOrigin_`, where `NoaliasScopeId` is a vector
  index that a nested emission renumbers.
- **Only `FullBuilderStateScope` is RAII; the other nine `SaveBuilderState()` call sites leak
  their bracket on a `LogError` unwind.** Such fixes belong in the shared struct, not in a
  hand-rolled save at the affected path.
- **Parentheses are a provenance-erasing token in this compiler.** A parenthesized primary is
  rebuilt in `ParsePostfixExpression` from a side channel carrying only type and storage, so
  every provenance flag is dropped - `q.p = (makeBox().t);` defeated a whole gate. Separately,
  `(T*)w` and `(T*)(w)` are DIFFERENT programs: the bare spelling materializes its own
  unledgered load inside `ParseCastExpression`. Probe both spellings; the bare one alone
  certifies dead checks.

## On changing approach vs. patching (additions)

- **When you decide "we cannot implement this, so carve it out", check whether REJECTING the
  carved-out shape was blocked by the same constraint - usually it is not.** Round 1 of
  the brace-list-globals-rejected record exempted `list`/`array`/`dictionary` globals because IMPLEMENTING
  global container construction was out of scope - and thereby preserved the exact filed
  silent-discard bug for those three types. Rejecting needed nothing the struct case lacked.
- **Prove completeness by ENUMERATING two sets and intersecting them, not by sampling.** The
  implied-move pointer guard lived in three copies and three successive rounds each fixed N-1
  of them (each unguarded copy a zero-output compiler SIGSEGV). What ended it: enumerating all
  19 `ConstantAggregateZero::get` sites against all 12 `MovableTempField` readers and showing
  exactly three intersect.

## On tests and measurement (additions)

- **When a fix corrects a TYPE whose wrong value is merely ABI-PERMITTED rather than
  guaranteed, assert the TYPE.** The `ftell`/`fseek` re-bound-to-`long` record's first regression test asserted
  `ftell(f) == 4` and was vacuous - the UCRT happened to zero the upper half.
  `typeof(rawEnd) == "long"` is what discriminates. This is the one documented exception to
  "assert VALUES, never it compiled".
- **An accept leg that passes on both binaries must be mutation-tested against the COMPILER,
  not against master.** Rebuild with the guard's polarity flipped, or the record site deleted,
  and confirm the specific legs go red (the closure-widen-gate join record and the
  boxed-object receiver-identity record both did).
  This is the only non-vacuity evidence available for a frozen accept set, and it names the
  defect the leg exists to prevent.
- **Leak counts and abort codes are functions of CACHE STATE, not just of the diff.**
  `Test/test_move.cb` is 13 leaks / 256 bytes warm and 15 / 304 cold, on pre- AND post-fix
  binaries alike; the recurring 2-leak "bitcode cache changes codegen" alarm was investigated
  and REFUTED - the real variable is the harvested libSystem stub flipping the
  `LC_BUILD_VERSION` sdk stamp (patching those 4 bytes reproduces the whole delta). Quote the
  cache state with every leak number; the `LC_BUILD_VERSION`-sdk-cache-independence record makes pre-2026-08-05 figures
  incomparable with later ones.
- **A runtime discriminator can be a property of the LINK, not of the program.**
  `MallocScribble=1`'s `1431655765` (0x55 fill) shows only in an `ld64.lld`-linked build; a PRE
  binary linked another way shows allocator-reuse values instead. Do not compare the fill
  across differently-linked binaries - re-prove a UAF with destructor counts plus a
  reallocation-aliasing witness. (When it applies, `MallocScribble=1` turns the temp-uniq
  family into a one-bit discriminator.)
- **Refinement of the deferred-diagnostic `expect_error` bullet:** the SCOPED form works for an
  end-of-body deferred check if the block wraps the ENTIRE function - it is a scoped block
  inside the function that closes first. Both spellings are in use
  (`err_unique_field_to_field.cb` wraps whole functions; `err_iface_field_missing.cb` uses
  scoped blocks because the check resolves at end-of-body inside the same hook).
- **The second brace SPELLING, cited so the next reader does not re-derive it:** `CFlat.g4:286`
  `initDeclarator: declarator '{' initializerList? ','? '}'` hangs its list on `initDeclarator`
  where `typeValue.Initializer` is null, so gating on `initializer->initializerList()` alone
  leaves it reproducing. Sprung three times (the brace-list-globals-rejected record, the
  struct-field-default-brace-list record, the non-aggregate-global-brace-list record); fixed
  structurally by `DeclTypeAndValue::BraceInitializer` +
  `FieldDefaultBraceList()`.

## On the differential corpus sweep (additions)

- **Both binaries must be in the SAME `--init-local` cache state, and cold is the state to
  compare in.** A warm POST against a cold PRE produced SEVEN phantom `core/*.cb` diffs, all
  "redeclaration of global" - which is what a warm core cache produces, not a rejection. Hit
  independently by four separate fix rounds (the multi-dimensional-bracket record, the
  code-VALUE-into-ANY-data-parameter record, the code-value-evidence join-ledger record, and the
  temp-unique-field-escape record).
  Equalize (or freshly rebuild) the cache on both sides before reading a single diff.
- **A COPIED exe is not a PRE binary.** The non-aggregate-global-brace-list record's first core sweep used a copied PRE
  binary whose `.cflat` cache still pointed at the other tree and produced 25 bogus
  circular-import/redeclaration diffs. Build a detached worktree at the merge base.
- **Capture rc into a variable BEFORE any command substitution.** `echo "$(basename $f) rc=$?"`
  reads `basename`'s exit code, reported a clean zero, and looked exactly like the answer the
  change wanted (recurred twice: the code-value-evidence join-ledger record and the
  closure-pointer generic-type-argument record, with `grep` at the end
  of a pipe). Same mistake as the `head` pipe, in a new costume.
- **Establish the noise floor by running the PRE binary TWICE before diffing PRE against
  POST.** Every reliable sweep did it (8-10 inherently nondeterministic rows: timings,
  addresses, pids). It doubles as the harness's non-vacuity check and is what lets a residue be
  dismissed honestly rather than assumed.
- **Report the sweep's COMPOSITION, not its file count.** One 447-file sweep broke down as 157
  compile-rejections, 208 `Test/errors/` fixtures (53 producing no binary at all), and roughly
  SIXTY programs that actually ran. The zero-difference conclusion was real and covered far
  less than "447 files" suggests.
- **The cheap detector for a non-crossing corpus is an INSTRUMENTED build that prints a marker
  whenever the new path fires.** A marker count of ~0 means the sweep proved nothing about the
  change, not that the change is safe (the value-keyed-boxing record's marker fired in exactly one file).

## Refinements to earlier bullets

- Refines "When a widely-read type flag changes, audit every guard that READS it": the trigger
  is not only a flag changing VALUE but a field becoming newly NON-NULL or a carve-out becoming
  newly REACHABLE. The simd-slot-splat record's recording of the simd array dimension sprang two such traps in
  one commit, one of which turned a clean hard error into a silent miscompile. Re-audit readers
  under the NEW conditions - a site classified from master's conditions is not audited.
- Refines "Do not reuse a predicate across a change of QUESTION": the mirror case is refusing
  to UNIFY two rules that look identical. The funcptr BIND site legitimately uses a looser
  arity rule than the ARGUMENT sites (`Test/test_program.cb` depends on it); the first attempt
  unified them and broke that test. **Do not unify the two arity rules.**
- Refines "Y is an ORACLE and must be verified": the same applies to reusing an existing HELPER
  as the oracle. `ParameterRetainsArgument` looks like the escape question
  the plain-`T*`-parameter escape-fact record needed; driven through a destructor-count oracle it called a
  local field store, `return n`, recursion and a vararg callee all "retaining" - every one a
  false rejection. Verified, then deliberately not used.
- Refines "A test that pins a PATH in `expect_error` breaks on the other platform": the
  mangled-NAME twin is the same hazard and it FIRED - two unrelated `expect_error` legs broke
  when `MangleTypeArg` changed. Prefix-pin, or pin the source spelling, until
  [[mangled-generic-name-leaks-into-diagnostics]] is fixed.

---

# Landed design records digest (from the retired interface-issue-queue.md)

One entry per landed/ratified record. Each captures the ratified behaviour and any approach
that was tried and must NOT be retried. Roughly chronological; entries are keyed by date and
description rather than by commit hash or branch name. The full records - with the original
branch names and commit hashes, coverage matrices, and verbatim witnesses - survive in git
history of `internal/issue/interface-issue-queue.md` before its 2026-08-08 deletion.

- **The 2026-07-28 session**: `as`/`is` fall-through replaced by `ClassifyCastSource`, a
  positive routing decision - the two fall-through shapes needed OPPOSITE answers (ternary must
  BOX, array must be REJECTED). Filed severity was wrong: `--run` JITs in-process, so the
  PROGRAM's SIGSEGV looked like the compiler's. Fixing named arguments on the interface path
  made call-site and declared-parameter indices diverge, exposing three downstream sites plus a
  false rejection and a `u8 200 -> -56` miscompile.
- **The 2026-07-29 session**: primitive-element array boxing - the mechanism was UNREACHABILITY
  behind a `StructImplementsInterface` early-out; the global/local divergence is purely
  Constant-vs-Instruction (a ConstantExpr bitcast verifies clean and detonates in
  SelectionDAG). `?:`-join interface return: boxing alone is not enough - each arm must be
  LEDGERED and each owning arm's source nulled in its own block. Duplicate-constructor crash:
  the filed "runaway recursion" guess was WRONG; the message's noun is picked by "declares a
  typeSpecifier" - do not "simplify" that back. Closure param widening must REJECT ONLY WHAT IT
  CAN PROVE IS DATA, never key off `isPointerTy()`. The `as`-boxing prerequisite (plumbing the
  source `NamedVariable` into `ParseTypeCheckExpression`) was built and verified
  behaviour-neutral BEFORE any guard was added - do it in that order.
- **Consolidation record (2026-07-30)**: 64 -> 58 issues, merged only where files named a
  shared ROOT and a shared fix vehicle, never a shared symptom; the four "namespace" gaps were
  deliberately NOT merged (registration scope, dispatch, lookup order, parser gap share only
  the word).
- **The return dangle, resolved on the fourth attempt**: never asks REACHABILITY; deferred
  to the end-of-body hook where the CFG is complete, an existential question over the returned
  local's use-list; every unrecognised user ACCEPTS. The null-store knob is `true` (null store
  = ACCEPT evidence) - `false` produced four confirmed false rejections. Rejected alternative,
  do not retry: a source-level "tainted binding" property (a missed assignment site becomes a
  FALSE REJECTION). Widening the extra-user whitelist is the direction that produced the
  earlier false rejections.
- **Generic-interface registration, resolved by record-then-resolve**: recording CANNOT reject,
  so a missed site degrades to no diagnostic. Four earlier shapes must not be retried: reject
  at end-of-compile over every syntactic occurrence (false-rejects uninstantiated template
  bodies); reject at each materialisation site (site enumeration failed twice, each miss a
  SIGSEGV); delete the check (reopens vtable laundering); and any at-site check at all, because
  "in `genericInterfaceInstances`, not in `interfaceTable`" is a legitimately TRANSIENT state.
  The struct-wins tiebreak must allow COEXISTENCE - the suggested "name in both maps" backstop
  `LogError` was deliberately NOT shipped.
- **Namespace key space, layer 1 (the template BASE)**: five ratified behaviour
  changes T1-T5 (bare spelling of a namespaced generic interface no longer reaches outside;
  inner scope wins for bare generic names; a namespaced generic struct no longer vetoes a
  global generic interface; templates nested in a struct now work; use-before-declaration now
  fails). Step 4 (key the struct-wins tie-break on the declaring module) was implemented,
  REVERTED and re-filed - "different module -> interface wins" contradicts ratified
  `Test/test_interface.cb` assertions; it needs a disambiguating spelling or a collision
  diagnostic, not a tie-break.
- **Namespace key space, layers 2, 3 and 4**: the one rule - a name must be
  RESOLVED once where its scope is current and the result RECORDED; never re-derive a scope
  from a key and never re-resolve a spelling downstream. Layer 2 resolves type ARGUMENTS
  through `ResolveTypeArgBaseName` whose accept set is TYPES ONLY; layer 3 resolves a
  substituted name from the ROOT (`forceRoot`), including six field-initializer sites found
  only in review; layer 4 adds `genericFunctionTemplates` to `IsGenericTemplateKey`.
- **The two P1s of 2026-07-31**: `IsProvableNonHeapAddress` is
  one-sided BY CONSTRUCTION - do not widen it. The array-element leg is keyed on GEP SHAPE, not
  on `ElementOwningUnique` (written in exactly one place, matches nothing here). DO NOT RETRY
  rejecting `function<T>*` wholesale - round 2 did, on the false premise that master's support
  was a constant-folding coincidence; the out-parameter has genuine store-through IR and the
  rejection was alias-bypassable. RATIFIED: `Pointer` is set on the function-pointer parser
  branch; `Lambda<T>[]` rejected while `Lambda<T>[N]` WORKS; `unique` on a funcptr/closure
  field rejected. Any `ParseDeclarationSpecifiers` fix touches four sites (direct + alias
  branch, in BOTH copies).
- **Non-heap addresses rejected at call sites, not just store sites**: a GLOBAL
  address is exactly as un-`free()`-able and as provable as a stack address, so
  `IsProvableStackAddress` became `IsProvableNonHeapAddress` in `LLVMBackend`; the check also
  runs on call arguments gated on `IsMove` or `uniqueAutoSink`.
- **Generic-substituted `unique` field ownership, and its destination gate**: STANDING HAZARD - `IsUnique`
  (written) and `IsUniqueTypeArg` (substituted) are two flags while destructor synthesis ORs
  them; any new ownership check written against `IsUnique` alone must be checked against
  `IsUniqueTypeArg`. Fix is `IsOwningUniquePointerField` re-keying the DESTINATION gate in BOTH
  field-store paths.
- **The SOURCE gate re-keyed onto `IsOwningUniquePointerField`**: closes what
  the destination-gate record left open. Ratified: reading a generic `unique` field out of a `move` PARAMETER now
  REJECTS instead of silently double-freeing - do not "fix" it back; `move other.t` is the
  correct spelling. Do NOT reflexively widen `IsUniqueFieldRead`'s GEP-shape test to reach the
  temp/call-result and container-element residues - each needs its own provenance signal, and
  widening over-matches a borrow read through a cast.
- **Separating `function<T>` from `function<T>*`; rejecting `Lambda<T>*`**: the
  scorer compares a three-state indirection SHAPE (`FunctionPointerShapeOf`: array / pointer /
  value); the promotion tier prefers fewer shape mismatches before the old move-score rule.
  `Lambda<T>*` (pointer to a FAT closure) is rejected at the declarator in both funcptr
  branches; thin `function<T>*` is unaffected. The shape marker rides the generated PREFIX
  (`cfuncptrPtr_`), not the tail, where it collided with a trailing pointer parameter.
- **Closure widening gated on the direct call path; interface argument slots
  type-gated**: the gate proves a MISMATCH; it does not require proof of a match. An earlier
  revision gated on "the scorer found no match" - WRONG POLARITY; scorer silence is absence of
  a rule, not proof of incompatibility. Both interface slot-picking arms must share the gate.
- **The fixed-array shape on the decl-init path**: TWO RATIFIED LANGUAGE
  DECISIONS, do not "fix" back - (1) `auto x = <fixed array>` deduces the VIEW `T[]`, a
  borrow, because `auto` introduces no storage; (2) `T[N] b = a;` IS a copy, lowered as a
  memcpy. The interface symptom was fixed by the DEDUCTION, not by widening the boxing guard
  (considered and rejected). The copy branch only INTERCEPTS an array-shaped source. On a fixed
  array the element's pointer-ness lives on `Pointer`, not `ElemPointer`. Whole fixed-array
  ASSIGNMENT is rejected; pointer-element `auto` is rejected rather than implemented.
- **Boxing keyed on the VALUE; the `??` join boxed per arm**:
  `RetireOwningSourceOfBoxedValue` is keyed on VALUE identity; widening `SoleCastOperandOf` to
  see through parentheses was explicitly REJECTED (a syntactic walk closes one spelling and the
  next binding-eraser reopens it). RATIFIED: the paren and `as`-through-paren spellings now
  MOVE their source. DO NOT RETRY a per-arm ownership transfer for the `?:` join -
  implemented, measured, reverted, and independently re-measured: it trades a double free for a
  use-after-null, and is unfixable IN PRINCIPLE because the consume-vs-borrow fact lives on the
  DESTINATION (a join into a plain interface local is a borrow by design).
- **`??` joins in RETURN and CALL-ARGUMENT position**: the
  return site reroutes through `UpcastPointerJoinToInterface` with
  `transferArmOwnership`/`armNotOwned` THREADED (dropping them silently regresses the working
  `?:` leg). For the ARGUMENT half there is no upcast site to reroute; failure is at SCORING.
  Do not retry the bare TypeName stamp: `IsTypeMatch` ignores `Pointer`, so a by-value class
  parameter scored a perfect match on a `Circle*`. The reviewer's suggested INVERSION ("bail
  unless every candidate is an interface") was evaluated and REJECTED. The pointer bail must be
  plain `param->Pointer`; narrowed to class-pointers it silently STOLE `f(void*)` / `f(char*)`
  / `f(int*)` calls.
- **Callee-side escape fact for the plain `T*` parameter
  (RATIFIED)**: `ParameterProvablyRetainsArgument` proves an escape only from a store whose
  destination outlives the call (global, caller-supplied memory incl. `this`, or a deref of
  either); UNKNOWN = ACCEPT is stated policy, and the walk is deliberately path-INSENSITIVE
  (three contrived correct programs now reject, accepted as the trade). Do NOT reuse
  `ParameterRetainsArgument` as the oracle (four measured false rejections).
  Record-then-resolve with `{callee, argIndex, name, access, FILE, line, col}`; one diagnostic
  per compile because `LogError` throws.
- **ONE borrow predicate for all five persist sites (RATIFIED)**:
  `SourceIsDanglingAliasBorrow` (`IsAlias || IsAliasBorrow`, minus `string`/closure/POD) is the
  single question at all five persist sites. A source-only rule must NOT be applied blindly: a
  by-reference lambda capture is also `IsAliasBorrow` and consuming it is CORRECT, so the two
  ADOPTING sites additionally require `BorrowAdoptionIsUnsound` (borrow lives in its own
  alloca/global slot).
- **Two distinct STACK-or-GLOBAL roots proved different (RATIFIED)**:
  `ProvablyDifferentObjects` strips the GEP chain and answers true only when both roots are
  DISTINCT `AllocaInst`/`GlobalVariable` - keyed on root KIND, never on `Value*` inequality.
  The issue file's preferred step 1 (give a global receiver a `CallerName`) was tried and
  REJECTED: the empty `CallerName` is load-bearing at several "this is a call result" checks
  and a dozen diagnostics.
- **The copy of an owning local aliased at its DECLARATION (RATIFIED)**:
  records `BorrowsOwningLocal` + `OwningLocalOrigin` + source slot at the declaration, decided
  by STORAGE IDENTITY not spelling; deliberately NOT `IsBorrowed` (~30 readers). The filed
  direction - arm it on the `alias` spelling - was measured and MUST NOT be retried: it
  false-rejects three programs master frees correctly, each with a LEAKING remedy. The proof
  retires at BOTH ends (`OwningLocalCopyStillAliases`); checking only the copy's own rebind
  false-rejected a correct program.
- **The four ownership facts a plain pointer COPY drops (RATIFIED)**:
  unique-field store (a store-side re-ask), container-element borrow (retires COPY-END ONLY,
  deliberately), `move` (destination-agnostic guard in `ParseMoveExpression`), and `?:`/`??`
  JOIN (`JoinKeepsOwner`, BOTH-ARMS rule, null literal arm neutral, MIXED join accepted).
  Reusing `InheritedKeepsOwner` for the join fact was tried and MUST NOT be retried -
  `MarkPointerRebound` also sets it on an implied-move store, which false-rejects a correct
  program. The arm proof records each arm's SLOT and re-asks it; recording only the NAME
  false-rejected seven correct programs.
- **A join arm PROVABLY parked at null is neutral (RATIFIED)**:
  `JoinArmIsProvablyNull` (null literal, or a load off a non-escaping alloca whose every store
  parks null, depth 3); its three refusals - no store, escaping slot, any non-null store - are
  the guard, and widening any to "could not tell" reopens the false-rejection direction. The
  boxed ledger needed a THIRD state (`SourceProvablyNull`), not a flipped bool; a PROVEN `move`
  on the surviving arm drops the whole fact.
- **2026-08-02 - one TYPE IDENTITY for overloads; the four sites a funcptr signature is read
  from**: `int` and `i32` are ONE overload, so declaring both is a redefinition error; canon
  lives only in `CanonicalPrimitiveSpelling`. Duplicate detection discriminates on source LINE
  and deliberately not on file (`currentSourceFilePath_` is unstable across LSP re-analysis).
  Do not unify the bind-site arity rule with the argument-site one. Do not trust a "this path
  is closed" claim that names a PATH rather than a SITE - all four sites read the signature
  independently.
- **Temp-source `unique` field stores; the implied-move guard in THREE copies**:
  the guard is required at the assignment, decl-init AND return paths; each unguarded copy is a
  zero-output compiler SIGSEGV, and three rounds each fixed N-1. Completeness was proved by
  enumerating all 19 `ConstantAggregateZero::get` sites against all 12 `MovableTempField`
  readers. `interface-field-self-assign-false-positive` was ATTEMPTED AND REVERTED - three
  discriminators must not be retried: variable NAMES, the interface locals' STORAGE, and a bare
  LLVM `Value` compare of the field address (each fat access re-loads, so even a true
  self-assign yields two distinct `LoadInst`s). Splitting the two-owner message on
  `MovableTempField` does not work; the discriminator is `OwningTempParent`.
- **Null interface access rejected at COMPILE TIME; runtime guard
  REJECTED**: RATIFIED BY THE MAINTAINER - reject as far as is provable, NO per-dispatch
  runtime null-vtable check, in debug builds or otherwise; `?.` is the language's answer. Zero
  codegen change, proved by byte-identical `--out-lli`. Rejection requires all three of: named
  local's own frame slot with plain `.`, address never leaves the frame, last write in the
  ACCESS'S OWN BASIC BLOCK is a whole-slot null store. Anyone widening the field axis must
  carry `Primary`'s defining `LoadInst` and require same-block - widening the anchor is a false
  rejection.
- **The array-view delete guard tracks the DECLARATION ONLY**:
  `ViewOfFixedArrayStorage` is set at the declaration only and ANY later plain `=` clears it
  permanently. Do not re-propose recomputing on reassignment (walk-order, not flow-sensitive -
  it false-rejected a correct program) and do not attempt full flow-sensitivity. Safe because
  `int[]*` is not a legal type, so `=` is the only rebind path. Cost accepted: one true
  positive stays accepted (`reassignHeapToStackNotDiagnosed`).
- **The thin `function<>` accept set; signature-aware binding**:
  the thin data-pointer hole is a LOWERING bug, not a scorer bug - the scorer's pointer clause
  STAYS permissive on purpose. The comparison is TYPE-CLASS level (`FuncPtrTypeClass`: i/f/p/v,
  0 = unknown), never SPELLING-level: a name-equality comparison was tried and hard-errored on
  six programs master runs correctly, and a corpus sweep structurally cannot see it.
  Signatures compare only at EQUAL indirection shape. A class-mismatched signature errors ONLY
  when no same-arity sibling can absorb the call.
- **The `CallerName` re-resolve only fires on a NAMED FUNCTION**: one
  condition, `llvm::isa<llvm::Function>(val)`, mirroring the two sibling re-resolve sites.
  Clearing `CallerName` on call results was NOT taken (it feeds `ScoreMoveAgreement`, the bond
  ledger and move tracking). A re-resolve test needs TWO OVERLOADS of the name to escape the
  single-overload early return.
- **`if const` leaf emission gated on insert-block LIVENESS**:
  `IsInsertBlockLive()` (non-null AND unterminated) replaces a non-null test; nothing clears
  the insert point at end-of-function, so at declaration scope the builder points at the
  previous function's terminated block. One predicate at the single leaf site covers all four
  `DecideIfConstCondition` callers; the tempting one-line "clear the insert point at
  end-of-function" is recorded as the wrong first move
  ([[insert-block-liveness-not-audited-repo-wide]]).
- **Brace-list globals rejected, both scopes and both brace spellings
  (RATIFIED)**: rejects a non-empty brace list on a struct/union/class/container global in
  BOTH spellings (`= {...}` and bare `{...}`, which live on different grammar nodes); bare
  NAMED local brace-init now works like the `=` form. Reject rather than implement: a global's
  initializer must be an LLVM `Constant` and neither the field-init nor the container path has
  a global counterpart. The round-1 container carve-out was wrong and was reverted. Ride-along:
  a primitive local with a value brace list now rejects (PRE read stack garbage); empty
  bare-brace zero-inits.
- **A brace initializer on a POINTER target rejected at four of five call
  sites (RATIFIED)**: four `EmitFieldInitializer` callers were broken (local declarator,
  fixed-array seed, default-parameter wrapper, `new T{...}` via generic substitution). The
  fifth - the named-argument site - was audited and found CORRECT; a first cut rejected there
  and removed a working feature. The tell: broken sites return packed FIELD BYTES (`0x1`,
  `0x900000001`), the correct one returns an ADDRESS. Rejection is role-named via
  `LogPointerBraceInitReject` so a test can prove which site fired; `int[] v = {1,2,3}` is
  `IsArrayView + Pointer`, so a bare `Pointer` test one branch earlier false-rejects it. Its
  `S*[N] a = {}` zero-init row is SUPERSEDED by the later empty-brace record.
- **Every unsized multi-dimensional bracket form REJECTED (RATIFIED)**: root
  cause is a dropped BRACKET (the grammar folds all pairs into one context, so empty pairs are
  invisible), not a dropped stride. The filed direction - carry `ConstInnerDimensions` on the
  view - cannot work and was NOT taken: a `T[][]` parameter has a per-call extent and a
  return/field/global has none; it fixes 2 of 11 miscompiling cells. One predicate at six
  sites. TWO working shapes are deliberately removed (`int[][] v = new int[10]`, and a 1-D
  array through an `int[][]` parameter) - they worked only because the bracket was dropped.
- **Empty `{}` split by TARGET TYPE (RATIFIED)**: `{}` yields a NULL
  `initializerList()`, so gates written on the list missed it. A NON-pointer `{}` seeds; a
  POINTER `{}` is REJECTED on an AMBIGUITY argument (null vs pointer-to-empty), which REVERSES
  the pointer-target brace-initializer record's ratified `S*[N] a = {}` zero-init row. Intended collateral, ruled on by
  the maintainer: a generic body's validity can now depend on its type argument (`T x = {}`
  with `T=S*` hard-errors); the rejection STAYS, since `T x = default;` works at every
  instantiation. `IsArrayView` is the ONLY exemption; a first cut also exempted `IsSimd` and
  stored a whole vector into an 8-byte pointer slot, aborting the compiler.
- **`ftell`/`fseek` re-bound to C's `long` (RATIFIED)**: Windows/LLP64-only
  defect; all three decl sites now say `long`. The fix is invisible in observed VALUES, so the
  test asserts `typeof(rawEnd) == "long"`. Standing notes: declaring the same extern twice
  with DIFFERENT types compiles clean and silently keeps the first (winner is import order),
  and `File` is capped at 2 GB on every platform.
- **A pure-rename `using` alias folded at MONOMORPHIZATION (RATIFIED)**:
  `list<MyInt>` and `list<int>` are ONE instantiation. The alias set is PRE-REGISTERED ahead of
  BOTH passes into a dedicated `manglingAliases_` map - a walk-populated map cannot work
  (`ScanGenericTypeUses` mangles before `ScanExternalDeclaration` sees the first `using`), and
  `typeAliases` is deliberately not consulted. `if const` arms and function bodies are
  deliberately NOT swept (`core/os.posix.cb` binds `win_size` to two widths). The new map is in
  the `--init` round-trip. PURE RENAMES only. Ratified tightening: `f(list<MyInt>)` +
  `f(list<int>)` now collides as a redefinition.
- **The last two funcptr-signature items (RATIFIED)**: the "make
  `FuncPtrParam.TypeName` qualified" direction was WRONG (that string feeds
  `BuildEncodedClosureName` and both passes must agree byte-for-byte); the resolved key went
  into SEPARATE fields and no mangled name moved. Narrowing is MEMBERSHIP-ONLY, and the
  ABI-canon hop must stay inside it or `Box<int>` vs `Box<i32>` false-rejects again. Inside a
  namespace, a bare spelling the walk could not qualify records NOTHING. The SCANNER copy is
  load-bearing. The `void*` gate keys on the ARGUMENT being code at
  `FunctionPointerShapeOf == 0` - a first cut keyed on "carries function evidence" and rejected
  `function<T>*` / `function<T>[N]` into `void*`, which are plain DATA pointers. Ratified:
  passing a `function<>`/`Lambda<>` VALUE to `void*` is an error.
- **A `simd<T,N>` slot that is NOT a vector no longer takes the splat
  (RATIFIED)**: "is `simd<T,N>*` supported?" is answered YES on measured evidence - answering
  NO (as `internal/simd-type.md` and a zero-hit grep suggested) would have false-rejected
  working parameter/global/field code. The decl-init splat is gated on the SLOT
  (`isa_and_nonnull<FixedVectorType>`), a ROUTING predicate not a rejection;
  `RecordSimdPointerAndDims` is shared by BOTH `ParseDeclarationSpecifiers` copies. Ratified:
  `simd<T,N>[N]` is real storage and `a[i]` is an ELEMENT (struct layout changes, `sizeof`
  16 -> 32); a scalar assigned into vector storage SPLATS every lane; a `simd<T,N>[N]` return
  is rejected like any by-value fixed-array return; `srcIsUnsigned` threads at all THREE splat
  sites.
- **A code VALUE no longer converts to ANY data
  parameter (2026-08-03)**: the gate is on the ARGUMENT being code (`ArgumentIsCodeValue`) and covers every
  data parameter the branch reaches, plus the implicit `char*`->`string` coercion. The oracle
  had two holes: variadic candidates are taken with NO per-argument scoring, so the gate is
  repeated in the variadic short-circuit over DECLARED parameters only; and
  `ParameterAcceptsCodeValue` must MIRROR the argument side's code shapes and must NOT be
  spelled from `IsEncodedClosureType` alone - that omits the literal `__closure_fat_ptr` a
  monomorphized generic parameter carries, which false-rejected a correct generic with a
  self-refuting message.
- **Name the guarding `if const` in the zero-implementor
  rebox error (2026-08-04)**: the conversion stays a HARD ERROR and the message names the class and the
  guarding arm chain; making it legal was considered and REJECTED (turns a compile error into a
  null-vtable segfault). Must not be retried: propagating uncertainty up the interface
  inheritance chain (tried and reverted); blaming classes under a generic TEMPLATE body
  (suppression only); using the last-component fallback of a qualified base spelling for BLAME
  (fabricated an implements-claim against an unrelated same-named interface). Twice-proven rule
  of thumb: over-broad candidate sets are SAFE for suppression and FABRICATE CLAIMS for blame.
  The registry is diagnostic-only and never serialized.
- **A temp's `unique` field may not escape the
  statement (2026-08-04)**: `IsOwningTempUniqueFieldEscape` at FIVE persist sites; `OwningTempParent` is the
  load-bearing half of the polarity (`FromOwningTempField` alone is also set for a BORROWED
  element - a gate keyed on it alone false-rejects correct code). The interface decl-init
  branch was missed on the first pass (its own ELSE), and PARENTHESES defeated the whole gate -
  fixed by widening the paren side channel, not the gate. Global scope is excluded on purpose
  so the truer pre-existing diagnostic survives. New leg sits AFTER the existing type-name leg.
- **A code VALUE no longer converts to a data pointer at a
  store (2026-08-04)**: nine gate sites along two axes read one shared destination-side predicate
  `CodeValueIntoDataDestination`. RATIFIED: `void* v = w;` is an error, agreeing with the
  `void*` PARAMETER rule. Do NOT retry the claim that three sites are "the whole set"
  (field/element/nested/global stores reach them only through the `=` OPERATOR; brace init,
  field default and parameter default are four more spellings); do NOT reuse the array-VIEW
  element-type derivation on the FIXED path (it silently disarms the guard for every `T*[N]`).
  Compound `+=` gets its own wording ("a code address is not an offset").
- **Receiver identity taken from the BOXED OBJECT,
  settled at end of body (2026-08-05)**: resolve each side's fat pointer back to the object its box wraps
  (`ResolveBoxedObjectOfInterfaceField` + `SoleStoreIntoSlot`) and feed the existing
  `ProvablyDifferentObjects`; RECORD at the store, settle at the end-of-body hook. Do NOT retry
  comparing variable NAMES (reverted; two names can denote one object), a bare `Value` compare
  of the field address (false-rejects the true self-assign - each access re-loads the fat
  pointer), or an at-site verdict (a loop can rebind the receiver afterwards).
- **A temp's `unique` field no longer escapes through a cast, a
  join, an array aggregate or a sink parameter (2026-08-05, RATIFIED)**: record-then-resolve keyed by value identity
  (`owningTempUniqueFields_`), read by the one predicate all five persist sites share. Do NOT
  retry "ANY arm answers yes" for `??` - the FALLBACK arm is never freed and rejecting it
  refuses a working program; do NOT gate on `FromOwningTempField` alone; do NOT touch
  `EmitGlobalFixedArrayInit` (a global keeps the truer pre-existing message). 40
  plain-`T*`-parameter cells and 10 `??=` cells left open by decision, not oversight.
- **A `?:` / `??` JOIN no longer erases the code-value
  evidence (2026-08-05)**: `codeValues_` + `codeValueDataCasts_`, read through `ArgumentIsCodeValue` so all
  nine store sites, the argument gate and the return gate are served from one source of truth.
  Do NOT hold the launder ledger for a whole function (round 1 did); do NOT "fix" it by
  reordering `isa<Function>` ahead of the launder check - mutation-tested, it false-rejects
  `c ? (Rec*)ro : n`. Widening was confined to `ArgumentIsCodeValue` and kept OUT of
  `ArgumentIsFunctionPointerish`, which the scorer's accept arm reads. RATIFIED:
  `void* v = c ? w : n;` is an error.
- **A `?:` / `??` JOIN no longer defeats the CLOSURE-WIDEN
  gate (2026-08-05)**: the declared mirror of the code-value-evidence join-ledger record; `dataValues_` + `JoinDeliversDataValue` with
  the OPPOSITE quantifier (every arm proven data, null neutral, unproven = fail-open). Do NOT
  copy `JoinCarriesCodeValue`'s any-arm polarity (mutation-proven to false-reject mixed joins).
  `RegisterDataValueCodeCast` must structurally refuse `ConstantPointerNull` and
  `llvm::Function` - statement scope alone was NOT enough. RATIFIED tightening: a `void*`
  holding a code address, joined, is refused; the remedy is the explicit `(function<...>)value`
  cast, and the message names it.
- **The emitted `LC_BUILD_VERSION` sdk no longer depends
  on cache state (2026-08-05)**: `sdkVer` resolved on BOTH branches, from `<cacheDir>/macsdk/SDKVersion`
  (written by `HarvestMacSystemStub` from `sysctlbyname("kern.osproductversion")`, NOT `xcrun`,
  so self-containment does not regress), trimmed to major.minor. `minos` stays `11.0.0`.
  Standing consequence: leak counts measured before this change are NOT comparable with counts
  after it.
- **A POINTER argument no longer binds a by-value
  parameter (2026-08-05)**: `IsTypeMatch` now refuses `Pointer && !other.Pointer` only - deliberately
  ASYMMETRIC and correct only at its one caller's operand order; `T` into `T*` keeps matching
  (a real capability, an implicit address-of proven from IR). Three doors, not one: the scorer,
  `DiagnoseProvableInterfaceArgMismatch` (virtual dispatch picks by ARITY and never consults
  the scorer; a by-value PRIMITIVE slot needed a second predicate because a primitive pointer
  argument carries an EMPTY TypeName), and `CheckIndirectCallArgShape` in both loops of
  `CreateIndirectCall`. Do NOT retry "the blast radius is zero" - `string*` into by-value
  `string` and a C by-value struct through the C binder both compiled and RAN before. Do NOT
  add a `Pointer` gate to the five `Parameters[0].TypeName == typeName` receiver-lookup sites.
  `Circle**` into `Circle*` deliberately left open
  ([[double-pointer-arg-binds-single-pointer-param]]).
- **Generic instantiation leaked its return TypeAndValue
  into the caller (2026-08-05)**: one-liner - add `TypeAndValue returnTV` to `BuilderState` and save/restore
  it with its three siblings. The filed "P2, nothing silently wrong" severity was wrong: a
  struct-value enclosing return is a compiler SIGSEGV and a `string` / owning-container
  enclosing return is a FALSE REJECTION blaming the user. The alias-scope registry
  (`aliasDomain_`, `aliasScopes_`, `viewScopeByOrigin_`) has the identical defect and was
  deliberately NOT folded in - it changes emitted alias metadata and needs an `-O2`
  verification pass neither suite performs.
- **A closure POINTER as a generic type argument: fat
  rejected, thin supported (2026-08-05)**: `ResolveTypeArgEntry`'s `functionPointerSpecifier` branch was
  dropping the `*` entirely (so `Box<Lambda<T>*>` and `Box<Lambda<T>>` resolved to ONE
  instantiation); three sites fixed, including the `functionTypeAliases` arm re-gated from
  `!hasPointer && !hasArrayView` to `!hasArrayView`, plus `RejectFatEncodedClosurePointerArg`
  at the pointer-suffix funnel. The filed repro had DRIFTED. `ClosureArgSpelling` is
  all-or-nothing about writability: a nested generic in the signature falls back to the raw
  encoded name rather than a hybrid.
- **Chained and nested joins boxed into an interface (2026-08-05)**:
  the filed direction - FLATTEN the chain at the ledger, splicing an inner join's arms into the
  outer entry - was measured WRONG and must not be retried: the inner arms' blocks are not
  predecessors of the outer join point, so the flat phi is invalid IR. What landed is RECURSION
  - box the nested join at its own join point (`CollectPointerJoinArms`,
  `NestedJoinArmsBoxable`, recursive `UpcastPointerJoinToInterface`). Flattening would also
  have silently changed four other ledger consumers, `JoinArmsKeepOwner` most sharply. Every
  cell moves reject -> accept; no accept-set cell changed (an earlier contrary claim is
  retracted as sibling inference).
- **A generic instantiated over a closure type lowers its
  element like the spelling it encodes (2026-08-05)**: the thin encoded closure's `{ i8* }` STRUCT backing
  was DELETED rather than patched at a third and fourth boundary; `GetType` resolves the
  encoded name straight to `BuildThinFnPtrType`. Do NOT re-introduce a wrapper representation.
  The `=` path's closure ownership-transfer arm is re-keyed on the REPRESENTATION
  (`IsFatEncodedClosureType`), not on `TypeName == "__closure_fat_ptr"` - the spelling key
  caused a review-caught miscompile strictly worse than the PRE rejection. One deliberate
  loosening (`Box<function<>>.item = vp` now matches its non-generic control) with the real
  hole filed as [[data-pointer-assigned-to-thin-function-value]].
- **A `sizeof` operand that looks like a generic type
  stays on the TYPE path (2026-08-06)**: `ParseUnaryExpression`'s character whitelist now admits `(`, `)`
  and `,` at generic-bracket depth >= 1 with balanced brackets. The filed framing was NARROWER
  than the truth - `sizeof(Pair<int,float>)` SIGSEGVs with no closure anywhere; the
  discriminator is the CHARACTER. Do NOT retry the obvious repair for the one cost cell
  (`sizeof(a<b,c>d)`): falling back to the expression path when the type lookup fails is
  precisely the fallthrough that reached `CreateCast` with a null `Primary` and SIGSEGVed.
- **A `class` with no user-written constructor
  default-constructs to zero, not `undef` (2026-08-06)**: one token, `Constant::getNullValue` instead of
  `UndefValue::get` at the class synthetic-ctor seed. The filed root cause (struct and class
  take different paths) was WRONG - same path, different seed. Shape 1 (do not register the
  ctor at all) was rejected on measurement: a generic container's own default ctor calls
  `_CNoCtor_CNoCtor__`.
- **The RETURN leg of the closure provenance gate (LANDED)**:
  `CheckClosureReturnProvenance` reuses `ArgumentIsProvablyDataPointer` so "pass" and "return"
  accept sets cannot drift; the THIN return arm had the identical hole and was not named in
  the issue file. Ratified tightening: a data-TYPED pointer provably holding a code address,
  returned as a closure, now hard-errors - matching the argument site, `(function<...>)value`
  escape hatch intact.
- **A struct FIELD's own `= { ... }` default brace list is
  applied, not discarded (2026-08-06)**: the issue file named `GenerateDefaultValue`, which is never
  reached; the five default-ctor emitters ask only `assignmentExpression()` / `Default()` and
  the grammar's third `initializer` alternative matched neither. Do NOT gate on
  `initializer->initializerList()` alone - the bare `Inner i { x = 1 }` spelling hangs its
  list on `initDeclarator`; `DeclTypeAndValue::BraceInitializer` + `FieldDefaultBraceList()`
  resolve both spellings. Step 3 (seed with the field type's OWN default ctor result) is what
  makes a PARTIAL list correct rather than merely non-zero.
- **The ASSIGNMENT leg of the closure provenance gate (LANDED)**:
  `CheckThinFnPtrAssignProvenance`, same shared predicate, SEVEN separate lowering paths -
  four of them found by review, not by the first round. The parameter-default gap can only be
  closed at the default-value site: the wrapper rebuilds the forwarding `NamedVariable` with
  the destination's TypeName, laundering provenance before the call-site gate runs.
  Guard-polarity bug caught by `./test.sh`: the new branches were missing the
  `!typeAndValue.Pointer` guard their pre-existing siblings carry, and false-rejected a
  POINTER to a thin slot.
- **The FAT twin of the default-value closure provenance gate (LANDED)**:
  `CheckFatClosureAssignProvenance` is check-only and does NOT widen. Widening was
  deliberately NOT implemented here: a LEGAL fat field default was measured already crashing
  on the PRE binary, so widening would have silently "fixed" a separate pre-existing bug
  without measuring its blast radius. Closed by the later record widening the field-default
  site to a legal fat closure source.
- **A brace list with values on a NON-AGGREGATE global REJECTED, matching
  local (LANDED)**: the guard was gated on `GetDataStructure(name).StructType != nullptr`, and
  an interface is not in the struct table - but the same hole swallowed primitives, `char*`,
  `function<>` and `simd` too; never an interface bug. The ORACLE (the local path) was
  verified independently on all eight spellings before being matched. No working program
  breaks: every newly-rejected cell already discarded its values. Deliberately not widened:
  empty `{}`, containers, and a container whose `StructType` is null (accept-on-doubt).
- **The FIELD-default site now widens a legal fat closure source (LANDED)**:
  `CheckFatClosureAssignProvenance` runs FIRST (throws on a provable data pointer) and
  `WidenBareOrThinToClosureFat` follows - two calls rather than one, so the existing reject
  wording and its legs stay byte-identical. Measured side effect recorded per the equivalence
  rule: the `(function<...>)` escape hatch's IR is NOT identical - PRE silently dropped the
  asserted address (`ret %D zeroinitializer`), POST emits the real `insertvalue` pair. Filed
  in passing: `S[2] a = default;` skips every field initializer while `new S[2]` runs them.
- **A bodyless PROTOTYPE no longer drops a fixed-array
  return size (2026-08-06)**: a bodyless prototype is not a `functionDefinition` at all (the grammar always
  requires a body), so it registers through `ParseDeclaration` and the definition-path reject
  structurally cannot see it. The guard is deliberately NOT keyed on `external` -
  namespace-scope, statement-scope and plain non-`extern` prototypes all reach the same arm. C
  interop is structurally unreachable (verified from source twice over, not from a passing
  probe): `RegisterCSignatures` calls `CreateFunctionDeclaration` directly, and
  `MapCTypeToTypeAndValueImpl` decays `[` and never writes `ArraySize`. The gate was NOT put
  inside `CreateFunctionDeclaration` - that layer cannot tell a user prototype from an
  internal wrapper registration.
- **An EMPTY brace pair is not an interpolation; a `string` reaching a
  `char*` parameter is diagnosed (LANDED)**: `HasInterpolation` and `ParseFormatString`
  disagreed about what a brace pair means; they are now one function (`ClassifyBrace`). Do NOT
  also move JSON-ish content (matched braces starting with `"` or `\`) to the plain literal
  path - the first cut did and broke `Test/test_reflect.cb::toJson_nested`; JSON-ish keeps
  `BraceKind::Verbatim`. Face 2 was NOT in the variadic guard as the issue file claimed - an
  interpolated argument 0 binds printf's declared `char* fmt` and a variadic candidate is
  taken without per-argument scoring; the guard is keyed on the REPRESENTATION (the named
  `string` struct type), never on `TypeName == "string"`.
- **An unresolved generic name no longer gets an opaque shell
  (LANDED)**: `AnyGenericTypeTemplateNamed`, accept-on-doubt, gating TWO copies -
  `ScanGenericTypeUses::tryPreDeclare` and `ForwardRefScanner::ParseDeclarationSpecifiers`;
  gating only the first leaves the headline repro compiling. The last-dotted-segment clause is
  load-bearing (without it three ratified `err_namespaced_generic_iface_*.cb` messages move)
  and is why the silent-accept face is NOT fully closed
  ([[last-segment-collision-still-shells-unknown-generic]]).
  `gts.scannedGenericStructNamesUncertain` is required and deliberately OUTSIDE the key space
  (an invented key is a false rejection); the shape that needs it is a function SIGNATURE. The
  filed claim about [[incomplete-layout-message-blames-c-interop]] causes was measured FALSE -
  it was a fourth, unlisted funnel.
- **A SELECTED shape-mismatched funcptr argument now rejects instead of
  lowering**: one shared `RejectFuncPtrShapeMismatch` running AFTER candidate selection, so
  multi-arm ranking is untouched. The parameter predicate mirrors the SCORER's
  (`IsFunctionPointer || IsEncodedClosureType`). Do NOT widen the shape-0-parameter face to
  `!= 0` - a shape-1 argument is already caught by `ArgumentIsProvablyDataPointer` with its
  own frozen wording. Do NOT re-derive an argument's shape from its symbol-table entry by
  `CallerName` - tried, and it false-rejected `arr[i]` (`CallerName` names the base array for
  both spellings); the fix is propagating `ConstArraySize` alongside `IsArrayView` in the two
  METHOD call-argument loops.
- **`??=` routed through the shared store tail (LANDED)**: `??=` now
  emits only the null test and branch, rewrites `operatorText` to `"="`, and falls through the
  shared tail (29 return sites via a `finishStore` lambda), so its RHS is a real
  `NamedVariable`. The desugaring `if (x == null) x = rhs;` is the ORACLE, but an UNSOUND one
  for facts that RETIRE a restriction: `ClearVariableBond` and `SetVariableBorrowsOwnedElement`
  take the JOIN under `??=`, deliberately diverging from `=`. `CoalesceRebound` is RETAINED
  and load-bearing. Do NOT derive the RHS owner from `DescribeAssignedSourceOwner(rightNV)`
  alone - it bails unless the RHS binding's storage is an alloca, and a CAST erases that; the
  first cut did and ran memory-unsafe (double free, rc 133).
- **A bare interface local with no initializer, genuinely uninitialised
  (LANDED)**: the discriminator is a whole-function existential fact - this (Base, empty Path)
  location has ZERO stores anywhere in F - reported with its own diagnostic
  (`ReportNullIfaceUninitAccess`), not the existing "last set to null" one, which would be
  false. Two conservatism axes: any-path-store disqualifier, and control-dependence
  containment (`cfg.Cd[accessBlock]` empty) - the latter added after review found the first
  cut REJECTED a guarded access that PRE accepted. Recorded fragility: the check infers "no
  initializer" from the absence of stores, and an interface PARAMETER's slot is structurally
  identical - it survives only because parameter lowering emits an entry store.
- **The borrow-forward policy, narrowed to explicit contracts
  (RATIFIED)**: the filed direction (`IsBorrowed && !BorrowedOrigin.empty()` as a third proof
  in the destination-agnostic move guard) was DISPROVED and must not be retried - it fires on
  `core/hpc/btree.cb::_rebalanceFrom`, which has no available remedy. The ratified policy is
  that the DESTINATION decides: a plain `T*` local does NOT adopt (keeps compiling, stops
  double-freeing), `unique` destinations and `move` RETURNS reject, and a `move` PARAMETER of
  a callee stays LEGAL. The borrow proof retires only via `BorrowProofRetiredByRebind` =
  `ReboundToOwnedValue && ReboundBlock == current block`. Two rejected retirement
  discriminators: bare `PointerRebound` (means "was assigned to", never "now holds an owner" -
  seven shapes became silent double frees) and `srcIsOwnedPtrRhs` carried whole (its syntactic
  `TopLevelMoveExpression` leg fires on the `move` token alone - four more silent double
  frees). Settled rule: **an ownership claim used for a RETIREMENT must be value-identity
  grounded, never syntactic.**
- **An ALL-DEFAULTED constructor is the type's no-arg constructor
  (2026-08-08)**: the filed root cause ("the declaration-site resolution between the user ctor
  and the synthetic no-arg ctor") was a hypothesis; measured, the crash needs no construction
  site at all - the DECLARATION alone kills the compiler, and every construction spelling was
  a red herring. `hasExplicitNoArgCtor` was spelled `!f->parameterTypeList()`, so `C(int x=3)`
  got a synthetic `C()` AND a cutoff-0 default-param wrapper claiming the same mangled symbol.
  `CreateFunctionDefinition`'s duplicate early return pushes NO function scope, and
  `GenerateDefaultParamOverloads` emitted anyway - so its `CreateBlockBreak(nullptr, true)`
  popped the CALLER's `stackNamedVariable` frame. That is the whole nondeterminism: SIGABRT /
  SIGSEGV / "declared with no enclosing scope" are three faces of one scope-stack underflow,
  and WHICH face appears depends only on what the freed frame held. Three parts landed:
  `AllParametersDefaulted` (grammar-level, `Ellipsis` excluded) widens both
  `hasExplicitNoArgCtor` sites; `ParseConstructorDefinition` takes the field-seeding branch
  instead of self-delegating when it IS the no-arg ctor (delegating would now be infinite
  recursion through its own wrapper); and `OverloadSlotIsDefined` decides BEFORE
  `CreateFunctionDefinition` rather than after. The pre-check is the load-bearing shape: an
  after-the-fact guard cannot work because `DiagnoseDuplicateFunctionBody` THROWS from inside
  the call whenever the two origins carry different lines - which is why the same program was
  a hard error written on two lines and a compiler crash written on one. `originLine == 0`
  (compiler-synthesized) yields silently; a real line is a genuine clash and gets its own
  message. Do NOT reuse the "redefinition ... type spellings" wording here: it is factually
  false when a default argument produced the overlap.
- **Widening WHICH types take an alternative lowering path means the alternative must be
  audited arm-by-arm against the one it displaces (2026-08-08, same commit)**: the
  all-defaulted-ctor fix routed a new population of types onto
  `ParseConstructorDefinition`'s in-line field-seeding branch. That branch honoured only
  brace-list and `= expr` field defaults; the synthesized default constructors it now stands in
  for ALSO default-construct a struct-typed field with no initializer and run
  `GenerateDefaultValue` for `= default`. The gap was a SILENT WRONG VALUE (`nested.v == 0`
  where the synthetic path gives 7), pre-existing for a bare `C() { }` and merely widened by the
  fix - reviews found it, the suite could not, because no in-repo type crossed the cell. Rule:
  when a routing predicate widens, diff the destination path's arms against the source path's
  arms as an enumeration, not by reading the one arm the bug report named. The oracle here is
  `ParseStructDefinition` :271-380; it was verified independently (a no-ctor probe prints
  `nested.v=7 nested.d.w=42` on master) before being copied, per "On the reference you are
  matching". **And the enumeration was STILL short by one arm** - the round-3 review found the
  synthetic path's type-MISMATCH arm (:342-369, narrow a scalar with `CreateCast`,
  default-construct a struct-typed destination) had no counterpart in the in-line branch, which
  silently drops the store instead: `u8 r = 200;` reads 200 with no ctor and 0 with one, and a
  generic `T val = 0;` instantiated at a struct reads 7 versus 0
  ([[inline-noarg-ctor-drops-mismatched-field-default]]). It stayed invisible through a whole
  round because the values people reach for first (`u8 r = 7`, `i16 s = 300`, `float f = 1.5`)
  all survive `Upconvert` unchanged - only a value needing a TRUNCATION crosses the cell. When
  the two paths are "the same arms", enumerate the destination path's arms from the SOURCE, and
  pick probe values that force each conversion the arm exists to perform.
- **`StructFields` is NOT index-parallel with the LLVM struct type for a UNION.** Every union
  member aliases one slot, so `%U = type { [1 x i64] }` has ONE element while `StructFields`
  has one entry per member. A field loop that computes `getTypeAtIndex(fieldIdx)` up front
  therefore indexes out of range on the second member - silent UB in a Release build. The
  pre-existing code was accidentally safe only because it reached `getTypeAtIndex` inside an
  `if (initializer)` that unions rarely satisfy. Bound any such loop by
  `structLLVMType->getNumElements()`.
- **The in-line seeding branch's type-MISMATCH arm, and what the six-site audit found
  (2026-08-09)**: `ParseConstructorDefinition`'s in-line branch now runs the same two arms as
  `ParseStructDefinition:342-369` - struct destination -> forceRoot exact-key `GetFunction` then
  `CreateOverloadedFunctionCall(TypeName, {}, true)` else `getNullValue`; scalar destination ->
  the existing narrowing `LogWarning` plus `CreateCast`. `LogWarning` (not `LogError`) is the
  right call here: it prints and RETURNS, and it already existed on the arm being mirrored.
  The oracle's casts were measured before copying, and one of them is counter-intuitive:
  int -> `bool` is a TRUNC, not `!= 0`, so `bool b = 5;` is `1` and `bool b = 6;` is `0`. The
  accept set was empty by measurement, not by argument: `core/` contains ZERO types with a
  user-written no-arg or all-defaulted constructor (an exhaustive body-scoped scan), and so does
  `example/`; the 454-file `Test/` + `example/` compile-and-run A/B found 39 raw diffs, all 39
  explained (31 = the compiler's own install path quoted inside "imported file not found",
  8 = wall-clock / concurrency counters / pid). The six-site audit's live findings: the CLASS
  synthetic emitter (`:2789`) and `ParseProgramDefinition` (`:2259`) both gate the mismatch on
  `&& destType->isStructTy()` and so have NO scalar arm - a mismatched scalar reaches
  `CreateInsertValue` unconverted and the ctor returns a struct constant whose element type does
  not match the struct type. `int i = 3.7;` reads `3` in a `struct` and `-1717986918` in a
  `class`. Filed as [[class-synthetic-ctor-drops-scalar-narrowing-arm]] and appended to
  [[program-field-no-initializer-skips-default-ctor]] rather than absorbed: their population is
  every type with NO constructor, a strictly larger accept set needing its own sweep.
  One cell is NOT a pure silent-wrong-to-right conversion, and review measured it: a SCALAR
  default on a FIXED-ARRAY field (`u8 a[4] = 200;`) inside a type with a user no-arg /
  all-defaulted ctor used to compile (store dropped, `a[0]` read 0) and now reaches the scalar
  arm's `CreateCast`, whose aggregate guard is a `LogError` - so it is a COMPILE ERROR, exit 1
  ("cannot store a single scalar value into fixed-array storage ..."). That is the convergence
  the fix is for, not a regression: the ctor-less twin already emitted the identical error, so
  post-fix all three spellings behave alike. Nothing in the 454-file sweep declares one. A
  brace-list array default (`u8 a[3] = {1,2,300};`) is a separate pre-existing hole and is
  genuinely unchanged - all zeros in every spelling, both binaries.
- **A lambda's by-value capture is owned by the ENV; the body's unpacked local is a borrow at
  runtime too (2026-08-09)**: the filed direction ("make the capture a real owning COPY at
  capture time") was already DONE and is not the bug - `--no-opt` IR shows
  `_copy_string_string_` at the capture store and a `__closure_cleanup_N` free arm, so the env
  genuinely owns its copy. The defect is that the INVOKER's unpacked local shallow-copied that
  value with its runtime OWNED bit still set, while the compile-time side already declared the
  local a borrow (`IsAliasBorrow`). Two halves, both load-bearing and mutation-proven separately:
  `ClearStringOwnedBit` / `ClearStructOwnedBits` on the unpacked value (fixes the REBIND face -
  `s = "abc"` inside the body freed the env's buffer, a double free with no return anywhere in
  sight), and a `copy` at the return when `NamedVariable::IsClosureValueCapture` is set (fixes
  the RETURN face, and is what makes the result INDEPENDENT rather than merely non-owning).
  Clearing the bit alone was measured and is NOT enough: it turns the abort into a SILENT WRONG
  VALUE wherever the env dies first (a closure local inside the returning function, a nested
  inner closure) - a crash traded for an empty string, the worse severity category. The
  discriminator that hid the whole thing is `llvm::isa<AllocaInst>(returnNV.Storage)` in
  `clearReturnedStringBorrowBit`'s `returnIsWholeLocal`: a capture's unpacked local IS an alloca,
  so it read as a movable whole-local. The same misread was already patched once for the borrow
  string PARAMETER (`IsBorrowStringParamStorage`) - a third instance should become a predicate,
  not a fourth disjunct. Sibling scope, measured not assumed: `list<T>` / `dictionary<K,V>`
  captured and returned are REJECTED by `SourceIsDanglingAliasBorrow` and never reach this path;
  a capturing lambda can never be thin (`function<T>` hard-errors), so there is exactly ONE
  capture-unpack site and one `EmitReturnExpression`. The `__closure_fat_ptr` carve-out twin
  (capture a closure, return it - rc 138/139) fell out of the same return arm via
  `IsOwningValueType`. Accepted trade, filed as
  [[rebound-lambda-capture-leaks-its-new-value]]: rebinding the capture inside the body now
  LEAKS the new value where it used to double-free it. Review caught the arm the fix did not
  measure: the flag rides through a FIELD read of the capture (`() => box.s`), where the copy
  fires but `clearReturnedStringBorrowBit` was already true (a field read is not a whole-local),
  so the freshly-copied buffer was re-tagged as a borrow and leaked - 64 bytes per call, 6.4 MB
  over a 100k loop, against 0 on the pre binary. An inserted copy must ALWAYS retire the borrow
  classification computed before it; the two arms are computed 300 lines apart and applied in the
  opposite order. The field arm is worth keeping: pre-fix it returned the EMPTY STRING once the
  env died first, so the copy converts a silent wrong value into a correct owned one.
  **The suite cannot see a regression of that retirement.** Verification mutation-tested it:
  with the two retirement lines deleted, `Test/test_function_ptr.cb` still passes 78/78 - the
  field-arm legs assert the VALUE and the buffer DISTINCTNESS, both of which the copy alone
  already provides, so they read as coverage they do not give. The only discriminator is
  `leaks --atExit` (100000 leaks / 6.4 MB on `scratch/rev_p2_sf_loop.cb`, 2 leaks / 32 bytes on
  the test file itself). This is the 2026-08-02 unique-field lesson recurring exactly: rigorous
  value legs beside an ownership change that only a resource count can falsify.

- **The 2026-08-09 indirect owning-source session** (`fix/owncopy`): `dest = src` and
  `T dest = src;` now defer to T for an INDIRECT owning source - `*ap`, `w.b`, `ot.inner.b`,
  `arr[i]` (fixed array AND view), `wp->b`, `wa.arr[i]`, `wa[i].b`, and a file-scope GLOBAL -
  exactly as they already did for a named local: copyable owner COPIES, non-copyable owner MOVES
  by zeroing the source lvalue. 30 measured corpus cells went rc 133 -> rc 0 with correct dtor
  counts. Ratified semantics: TRANSFER, not a `.copy()` rejection - `move w.b` already nulled a
  field lvalue, and the deref-DESTINATION arm already consumed a field SOURCE, so rejecting would
  have contradicted two shipped behaviours and the plan's "`=` is total over T". Only a NAMED
  slot is `MarkVariableMoved`; an indirect lvalue is consumed silently (there is no spelling to
  report a later use of). The global cell was a separate miss inside the same arm:
  `GetGlobalVariableNV` sets no `CallerName`, so the old `!CallerName.empty()` slot test never
  matched a file-scope global - a static local (a module global WITH a name) always worked.
  **Do NOT route `string` through the widened arm.** The first cut did, preempting the dedicated
  `srcBorrowsOwnedString` deep-copy-on-borrow branch in decl-init; every suite stayed green and
  `Test/test_move.cb` still passed 831/831, while `leaks --atExit` on the UNCHANGED master test
  file went 16 leaks/320 bytes -> 17/336. `string` ownership is a RUNTIME owned bit with its own
  machinery; the family here is owning STRUCTS. Two destination-side twins were measured and
  deliberately left out, with issue files: a FIXED-ARRAY element destination is broken for every
  source shape including a named local (its slot is LIVE, so it needs drop-old - the opposite of
  Part 6's container slot, which must not), and `o = *op` with `op == &o` self-consumes to null.
  The container single-index-GEP source/dest gate was NOT widened, per the plan's LOAD-BEARING
  INVARIANT (list `sort`/`_partition` bit-shuffles and dictionary rehash depend on it); decl-init
  from a single-index GEP is excluded for exactly that reason, which is why
  `T tmp = _data[i];` still borrows.

- **The 2026-08-09 fixed-array-element DESTINATION session** (`fix/arrslot`), the destination-side
  twin of the bullet above. `dst[i] = src` on a fixed array of owning structs was a plain bit
  store for every source shape (rc 133 for an lvalue source, a silent leak of the old element for
  a temp/call-result source). The classification is the whole fix: a FIXED-array subscript is a
  **TWO-index GEP over `[N x T]`**, so it is neither `destIsStructField` (2-index over a STRUCT)
  nor `destIsLocalOwningVar` (alloca/global) nor Part 6's SINGLE-index container slot - it needs
  its own accept set (`destIsFixedArrayElem`), and Part 6's gate stayed untouched per the plan's
  LOAD-BEARING INVARIANT. A fixed array's slots are LIVE default-constructed values, so this
  destination DOES drop-old; a container slot must not. That difference is the reason the two
  gates can never be merged, however similar the spelling looks.
- **Zero the SOURCE before destructing the DESTINATION - the ordering IS the self-assign fix.**
  `dst[i] = dst[i]` cannot be ruled out at compile time for a runtime index, and pointer-identity
  guards (`destination != rightNV.Storage`) see two different GEPs / a loaded pointer. Producing
  the value, zeroing the source, and only THEN running the destination dtor makes every aliasing
  spelling degenerate safely: the dtor finds a zeroed slot, frees nothing, and the store puts the
  value back. Measured on `dst[i] = dst[j]` with `i == j` at runtime, `dst[0] = *p` with
  `p == &dst[0]`, `base[0] = v[0]` through a `T[]` view, and `m[1][1] = m[1][1]` - all keep the
  value with zero frees. This is exactly the ordering
  [[self-assign-through-a-pointer-to-the-destination-drops-the-value]] needs: the LOCAL arm's
  dtor -> store -> zero order is what loses the value there, and the element arm shows the cheap
  fix is to reorder rather than to build a same-object proof.
- **An arm that RETURNS EARLY inherits responsibility for every diagnostic downstream of it.**
  Review of this fix found the new element arm gated on `IsOwningValueType(source)` with no
  type-equality check, so `Nest[2] arr; arr[0] = aBox;` (an `ArrElemBox` value into an
  `ArrElemNest` slot) went from a clean master reject to compiling into a mismatched store - the
  arm returned before the cast diagnostic could fire. The drop-old arm did NOT regress, because it
  falls through. Fixed in review by requiring
  `ResolveTypeAlias(src) == ResolveTypeAlias(dest)` (alias-resolved, per the `using` lesson) and
  pinned by an `expect_error("cannot cast an aggregate value")` leg in `err_move.cb`. **When an
  arm ends in `return`, the accept-set corpus must include the ILL-TYPED neighbours, not just the
  well-typed ones** - a positive corpus of 28 same-type cells cannot see a lost rejection. Note
  the whole-LOCAL arm has the same hole (`Nest m; m = n.inner;` compiles on master too); it was
  left alone as pre-existing rather than absorbed.

- **The 2026-08-09 fixed-array BRACE-INIT session** (`fix/bracown`), the decl-init twin of the two
  bullets above. `T[N] dst = { a, b };` bit-copied every owning lvalue element into the slot (rc
  133). The whole fix is one new helper, `ConsumeOwningBraceElementSource`, and one call at the end
  of `EmitPositionalFixedArrayIntoSlot` - which is why BOTH of the emitter's callers (the local
  declarator, for `= {...}` and the bare `{...}` spelling, and `EmitFieldDefaultFixedArrayBrace`)
  were measured independently broken and independently fixed by it. RATIFIED: the brace element is
  CONSTRUCTED, so it takes the CONTAINER-slot decision, **no drop-old** - the emitter zero-fills the
  whole array first, and the freed-once-each dtor counts are the only thing that discriminates a
  transfer from an alias. Otherwise it is the same decision as the two siblings: copyable owner
  COPIES (distinct buffers, sources live), non-copyable MOVES and zeroes the source lvalue, and only
  a NAMED slot is `MarkVariableMoved`. The repeated-source list `{ a, a }` therefore resolves as
  `use of moved variable 'a'` on the second element - that is the answer, not a special case. The
  INDIRECT repeated list `{ w.b, w.b }` is the documented silent half of the same rule: the first
  element consumes, the second stores a null item, rc 0 and leak-clean, with no diagnostic, because
  an indirect lvalue has no spelling to report.
- **A `return`-ing arm needs its ill-typed neighbours in the corpus - and the review has to probe
  them in the ARM'S OWN SPELLING.** The arrslot bullet above learned this for `dst[i] = src`; the
  brace arm shipped with `ResolveTypeAlias(src) == ResolveTypeAlias(dest)` already in place because
  of it, and review confirmed by probe that `Nest[2] arr = { aBox };` still reaches "cannot cast an
  aggregate value" identically on both binaries. The `using` alias and generic-instantiation
  spellings (`BAlias[2] dst = { a };` in both alias directions, `GBox<int>[2] dst = { g };`, and the
  ill-typed `GBox<float>[2] dst = { g };`) were probed as well and are all correct - alias-resolved
  comparison is what buys this, and it is now the third fix in a row where it was load-bearing.
- **A field DEFAULT that names a global owner is a one-shot.** `struct W { Box[2] arr = { gSeed };
  }` consumes `gSeed` at the FIRST default construction; a second `W w2 = default;` gets a null
  element (rc 0, leak-clean; rc 134 before this fix). Memory safety gained, value silently lost -
  recorded on [[implicit-consume-of-a-global-owner-loses-the-value-on-the-second-run]] as the third
  and worst-reading spelling of that defect, because a field default is written once and runs per
  instance, so nothing in the source hints that it is one-shot. It also means a test leg that reads
  such a field default goes VACUOUS the moment a second construction of the type is added to the
  file - `Test/test_move.cb`'s `abri_fielddefault_*` legs depend on being the only one.
- **The by-value owning PARAMETER was the shape the accept-set matrix missed.**
  `void f(UBox p) { UBox[2] d = { p }; }` double-frees (rc 134) while `d[0] = p;` is clean, on BOTH
  binaries - not a regression, but the one source shape a 30-cell corpus of locals, fields, derefs,
  elements and call results never reached. The two halves disagree: the arm consumes the parameter,
  the pre-pass that decides a parameter is a CONSUMING parameter does not know the brace-list site,
  so the caller is never told and destructs a resource the callee already freed. Filed as
  [[brace-init-element-consumes-a-by-value-parameter-the-caller-still-owns]]. The general lesson:
  when an ownership arm is added to a new lowering, the corpus must include a source whose OWNER IS
  ANOTHER FRAME - consuming it is only half a transfer.
- **A brace element and an assignment can be the same store and still not be the same path.** Both
  string-element spellings were measured broken and, being one defect, the two issue files were
  merged into [[fixed-array-string-element-store-double-frees]] at the higher severity (the p2
  assign-only file from `fix/arrslot` was deleted). `string` stays excluded from all three arms:
  routing it through `ClassifyOwningAssignSource` is what `fix/owncopy` measured as silently
  leak-adding while every suite stayed green.
- **A call that yields NO llvm::Value must be refused at the CALL, not at each consumer**
  ([[void-closure-call-result-consumed-reads-garbage]], fix/voidcall). `CreateIndirectCall`
  already returned `nullptr` for a void invoker and the result `NamedVariable` already carried
  `TypeName == "void"` - the defect was entirely downstream, and it wore five different faces on
  the SAME construct: silent garbage (declarator init `r=-16`, `?:` arm, variadic arg, thin
  `function<>` `r=-77135616`), a silently skipped call (`int gr = gg();` at global scope printed
  `gr=0` with the closure never invoked), a dropped argument reported as the false "no overload
  of 'take' matches ... Call arguments (0)", a locationless `Module verification failed: Operand
  is null` (condition position), and a COMPILER SIGSEGV with no output at all (assignment, field
  store, element store, binary operand, call through a closure parameter). Enumerating consumers
  would have needed the nine-site destination checklist plus the four argument doors and still
  left the unenumerated ones silent; gating the producer is ONE site
  (`MainListener_PostfixExpression.cpp`, the sole `CreateIndirectCall` caller in the listener)
  and no consumption position can escape it, because the value never comes into existence.
- **The exemption set for a value-less result is a closed language rule, so name the POSITION
  rather than the site.** `ResultUse { Value, Discard, ReturnOperand }` replaced the old
  `bool discardResult` and rides the same pure single-child passthrough chain
  (`ParseAssignmentExpressionNamed` -> `ParseCastExpression` -> `ParseUnaryExpression` ->
  `ParsePostfixExpression`), which resets to `Value` in every operator context - so
  `return g();` defers to `EmitReturnExpression` while `return g() + 1;` does not. Both
  exemptions are mutation-proven: flipping `ReturnOperand` back to `Value` falsely rejects
  `void f() { return g(); }` (the `rvx_closure_void_crossing` accept leg) AND breaks
  `err_return_void_from_value.cb`'s `rvbClosure` leg by changing its message; flipping either
  `Discard` site to `Value` falsely rejects a for-increment call and a void `=> expr` body.
  The [[return-value-void-mismatch-fails-module-verification]] closure detection in
  `EmitReturnExpression` (`right == nullptr && TypeName == "void"`) is NOT made redundant by
  this - deleting it still breaks the `rvbClosure` leg, because the call site now hands that
  position through untouched.
- **The DIRECT spelling is a different defect and was left alone deliberately.** `int r = f();`
  on a void `f` yields a real void-typed `llvm::Value`, builds its result `NamedVariable` at a
  dozen `lastCallReturnType` sites rather than one, and still fails as `Module verification
  failed: Invalid bitcast` (declarator), `Both operands to a binary operator are not of the same
  type!` (arithmetic) or rc 133 (`auto r = f();`). Measured pre and post, byte-identical. Do not
  read those dumps as the oracle the closure path was converged onto - they are the same
  defect shape in the direct path, and converging them needs its own gate. Filed in review as
  [[direct-void-call-result-consumed-fails-verifier]] (P1: six of eleven positions are
  locationless verifier dumps, `auto r = f();` is rc 133 with no output, and a void INTERFACE
  method through the vtable lands there too).
- **A `ResultUse`/discard thread that rides only the single-child chain stops at a
  parenthesis.** Review of the same fix found `(g());`, `((g()));`, `cond ? g() : h();` and
  `() => (g())` newly rejected - all four compiled and ran on the base. The pre-existing
  return-block gate has the identical hole (`(RbA(1));` is rejected on master), which is what
  makes it one issue rather than a regression class:
  [[discard-position-not-threaded-through-parens-and-ternary]]. When a gate keys on a threaded
  position, probe the WRAPPERS (parens, `?:`, cast) as well as the positions - the wrapper is
  where the thread is dropped, and the accept-set corpus will not show it if every leg is
  spelled without one.
- **A gate keyed on a literal TypeName misses the `using` alias, again.** The same review found
  `using V = void; Lambda<V()> g = ...; int r = g();` still returning garbage (`r=586762112`)
  with the gate in place, because it compared `TypeName == "void"`. Fixed in review by comparing
  `ResolveTypeAlias(TypeName) == "void"`, with `vccAlias` / `vccAliasThin` legs added to
  `err_void_closure_call_consumed.cb`. This is the 2026-08-02 `using Cb = ...` lesson recurring
  verbatim: whenever an acceptance or rejection rule matches a type SPELLING, the alias spelling
  is a required leg of the corpus, not an exotic one.
- **Round 2 of the same fix: the paren half of that thread was closed, the `?:` half was not,
  and the stopping point was the API boundary.** `ParsePrimaryExpression` now carries the
  `ResultUse` and forwards it through `'(' expression ')'` only, with
  `ParsePostfixExpression` handing its own `use` down when the primary IS the whole postfix
  (`childLimit == 1`). That restored `(g());`, `((g()));` and `() => (g())` to the base's rc 0.
  The `?:` arms live behind a different API (`ParseConditionalExpression` /
  `ParseTernaryBranches` / `ParseExpression`, all `TypedValue`, plus the eager constant-context
  fallback and the join ledger), so threading a position into them is four more signatures and a
  semantic decision about what a discarded arm means to the join - the re-enumeration shape, and
  it was stopped rather than forced. **The paren threading also closed the pre-existing
  return-block paren hole** (`(RbA(1));`, rc 1 on the base, rc 0 now). That was shipped only
  after checking SEMANTICS against the plain spelling as oracle, not the exit code: a trace
  counter over both polarities shows `(RbA(a));` behaves identically to `RbA(a);`, i.e. the
  inlined `return` still exits the caller.
- **A mirror predicate needs the alias fix at the same time as its twin.** Review fixed the
  call-site gate to compare `ResolveTypeAlias(TypeName)`; the mirror in `EmitReturnExpression`
  (cd6533c's `right == nullptr && TypeName == "void"`) still compared the literal, so
  `int f(Lambda<V()> g) { return g(); }` under `using V = void;` was a locationless
  `Module verification failed` on the base AND after the review fix. Found by probing the ALIAS
  axis across POSITIONS rather than only at the site that had just been patched. Both are now
  resolved; leg `rvbAliasClosure` in `err_return_void_from_value.cb`, mutation-proven. Note the
  accept side (`void f() { return g(); }` through the alias) passes either way - it works
  because `right` is already null - so only the REJECT leg discriminates that line.
- **The 2026-08-09 pointer-depth mirror**: `TypeAndValue::PointerDepth` (an int, `0` = NOT
  RECORDED, model caps at 2) is the POSITIVE half of the proof the boolean `ElemPointer` could never
  give, and it is what lets the mirror gate refuse a `T*` argument at a `T**` parameter.
  **Do NOT retry the mirror on `!ElemPointer`** - a `T*[N]` slot, an inline `&a` and a generic
  substitution all carry byte-identical `arg{Circle p=1 ep=0}` to the broken call, measured. It is
  written at exactly five producers - both `ParseDeclarationSpecifiers` declarator branches, `&`
  (+1 over an ALREADY-RECORDED depth, never inventing one), `*` (-1), and the pointer-buffer
  subscript (-1) - and rides both `--init` round-trips as `"pd"`. **Every DEPTH-REDUCING producer
  must be found before the gate ships**: the subscript one was missed on the first cut and
  false-rejected `byPtr(buf[0])` over a `T** buf`, a program master runs correctly; the suite was
  green (630/0) with that false rejection in it, and only a hand-written probe caught it. Recording
  depth on `&` also changed OVERLOAD SELECTION, not just post-selection validation: `pdPick(&a)`
  over `{f(T*), f(T**)}` moved from the `T*` body (garbage) to the `T**` one, which closed section
  1 of the residue issue. `IsProvenSinglePointer` (the landed gate's PARAMETER side) was
  deliberately NOT switched to read the new field: params synthesized by C interop and WinRT never
  set it, so requiring it there would silently retire existing rejections.
  **A CLAMPED depth is a FALSE claim, so over the cap record `0`, not the cap**: the first cut
  wrote `min(stars, 2)`, which made a `T*** ppp` declarator claim 2; the `*` producer then stepped
  it down to a positive 1 and HARD-REJECTED `byPP(*ppp)`, a program master runs correctly. The
  whole suite AND the fix's own 30-file corpus were green with that false rejection in them - the
  same shape as the missed subscript producer, caught only by a reviewer probe. Any approximation
  that feeds a POSITIVE proof must degrade to "not recorded", never to a nearby number (frozen as
  the value leg `pd_deref_of_triple_ptr_into_ptrptr_param`).
- **The 2026-08-09 owning-sink SITE lesson (`fix/parmbrace`)**: a callee-side consume arm and the
  caller-side sink inference are TWO halves that must be extended together. `fix/bracown` taught
  the fixed-array brace element to consume its source; the caller half lives in
  `CollectConsumedStoreNames` (`cflat/MainListener.h`), a purely SYNTACTIC scan that fed
  `ApplyOwningSinkInference`, and it only knew three source spellings - an `=` RHS, a decl
  initializer's `assignmentExpression`, and `move <name>`. A brace list is an `initializer` with
  NO `assignmentExpression`, so `void f(UBox p) { UBox[2] d = { p }; }` consumed the parameter in
  the callee and never made it a sink: caller kept `a` live, both freed, rc 134, while the
  assignment spelling `d[0] = p;` was clean. Fix is one small helper - collect the POSITIONAL
  `fieldInit` texts of a brace list, from BOTH `initDeclarator` alternatives (`= { ... }` and the bare
  `d { ... }`, which hangs its list off the declarator). **Whenever a new consuming lowering lands,
  grep `CollectConsumedStoreNames` and ask whether the new source spelling is one of the three it
  recognizes.**
  Two properties made the one-line-per-site fix safe and are worth reusing: (a) the scan is
  deliberately OVER-approximate - marking a sink that the body does not actually consume is sound,
  because 8a's total scope-exit drop frees the callee's copy anyway (proven by the conditional cell:
  `if (c) { UBox[2] d = { p }; }` with `c == 0` now frees in the callee, exactly as the assignment
  oracle already did); and (b) the STRUCTURAL flag `IsConsumeInferredSink` is filtered later by
  `OwningSinkConsumesConcrete`, so a copyable owner stays a borrow with no extra guard here.
  `osk`/`cis` already ride the `--init` round-trip, so no serializer change was needed - cold and
  warm cache measured identical on the repro.
  Named struct brace-init (`Pair s = { b = p };`) is NOT in this family: it already REJECTS with the
  "copying owning value ... into a struct field" diagnostic, and positional struct brace-init does
  not exist. Only the positional form is collected, for that reason.
  Two neighbours were measured broken in BOTH spellings and filed rather than fixed: a field of a
  by-VALUE struct parameter (`{ w.b }`, rc 134 - the explicit `move w.b` already rejects it, the
  implicit stores do not) and lambda parameters, which never reach `ApplyOwningSinkInference` at
  all. **When the oracle spelling fails the same way, the cell is a different bug** - mirroring it
  would have propagated the abort, not fixed it.
  The review found a THIRD such neighbour and filed it: a PARENTHESIZED source `(p)` records the
  text `"(p)"`, misses the parameter-name intersection, and aborts in all FOUR spellings on both
  binaries (`p1/parenthesized-consume-source-defeats-owning-sink-inference.md`). That is the
  over-approximation running the UNSOUND way - over-collecting a name is safe, MISSING one is a
  double free - so any future edit to these collectors must be checked in that direction too.
- **The 2026-08-09 bare-`string` fixed-array element (fix/strelem)**: a `string` element of a
  fixed array is neither a struct field, nor an alloca/global local, nor Part 6's single-index
  container slot, so EVERY dedicated string arm skipped it and the plain aggregate store aliased
  the `{ptr,len}` pair - source and element both carried the owned bit, both freed, rc 133 - while
  an overwrite orphaned the old buffer (16 bytes under `leaks`). Both spellings were broken, and
  both were fixed with the same shape: `ParseAssignmentExpression` grew a `string` twin of the
  element-transfer arm (deep-copy an lvalue source, drop the old element, store, early return), and
  `ConsumeOwningBraceElementSource` replaced its `elemTypeName == "string"` early return with a
  deep-copy-only leg (the slot is CONSTRUCTED, so no drop-old) plus `UnregisterOwnedStringTemp`.
- **Gate a `string` arm on the REPRESENTATION, not the spelling.** The first cut of that arm
  compared `rightNV.TypeAndValue.TypeName == "string"` and silently missed the overwrite-leak
  cell, because an `operator+` temp's `NamedVariable` carries NO `TypeName` at all. The landed gate
  is `right->getType() == StructType::getTypeByName(context, "string")` on the source plus
  `NamedVarIsString(namedVar)` on the DESTINATION - and both halves are load-bearing. The
  destination half is what keeps a user struct that happens to share `%string`'s `{ptr,i32}` shape
  out of the arm; the LLVM named-type identity is what keeps `using S = string;` IN it, since
  `NamedVarIsString` already falls back to `BaseType`'s struct name. Reviewer-measured: a
  same-shaped user struct in its own fixed array is untouched, and both alias spellings
  (`S[2] dst; dst[0] = t;` and `S[2] dst = { t };`) went rc 133 -> rc 0 with distinct buffers.
- **Copy BEFORE dtor for an element store; dtor-before-copy is only safe where a compile-time
  self-assign guard exists.** The whole-local and struct-field oracles destruct the old value
  first because `destination != rightNV.Storage` proves the two are distinct. A fixed-array
  element has a RUNTIME index, so no such proof exists: `dst[i] = dst[i]` would deep-copy a buffer
  the destructor just freed. The landed order is deep-copy, then dtor, then store. Note this is the
  MIRROR of the non-string element arm two blocks above, which moves/zeroes the source first and
  destructs after - same self-assign hazard, opposite resolution, because a move must not be
  undone by the drop-old. Do not "harmonize" the two orders.
- **The container mis-taint under an element access is SIDE-STEPPED, not fixed.** Both the head
  `MarkVariableOwningString` (MainListener_Expressions.cpp:1361) and the tail
  `SetVariableBorrowsOwnedString` refresh (~2656) key on `namedVar.CallerName` with an empty
  `FieldName`, which for `dst[0] = ...` is the ARRAY `dst`, not the element. It is unreachable
  today only because the new arm returns early; measured benign where it does fire (the array
  teardown emits one dtor per element either way, and elem0's GEP folds to the array alloca).
  Deliberately NOT filed as an issue - there is no spelling that observes it - but any future arm
  that FALLS THROUGH to the tail re-arms it.
- **Making an element genuinely own its buffer exposed the next domino: the element READ.**
  `string q = dst[0];` shallow-copies the element's owned `{ptr,len}` into the new local, so both
  free - rc 133 on master for a local-variable or concat-temp source, and rc 0 on master ONLY when
  the element was an alias of something else. Closing the store aliasing turned that accidental
  rc 0 into rc 133 for the field-source spelling. That is the fix being right, not a regression:
  filed in review as [[fixed-array-string-element-read-aliases-the-element]]. When a fix converts
  "the slot never really owned anything" into "the slot owns", re-probe every READ of that slot.

- **`fix/parenmv` - the parenthesized consume source, and where a text peel stops working.**
  Closing the round above: `BareSourceText()` in `MainListener.h` walks down through single-child
  nodes and through the `primaryExpression : '(' expression ')'` alternative, and all five
  name-recording sites in `CollectConsumedStoreNames` / `CollectUnconditionalMovedNames` /
  `CollectPositionalBraceElementNames` now record its result. Descending a single-child chain never
  changes `getText()`, so the peel is text-preserving for everything except real parentheses - which
  is why `(v.f)`, `(v + 1)`, `(f(1))` and the tuple `(a, b)` still fail the intersection, as they
  must. All four consuming spellings, at paren depth 1-3, now match their bare control exactly,
  caller-side `use of moved variable` rejection included.
  The instructive part is what the peel could NOT fix. Three measured cells stay wrong and were
  filed as `p1/parenthesized-operand-loses-named-variable-provenance.md`: `return (p)` of a borrowed
  by-value owning parameter (rc 134 vs rc 0 bare - the caller allocates a second owner and destructs
  it), a CONDITIONAL `move (p)` of a COPYABLE owning parameter (rc 134 vs rc 0), and `UBox o = (x);`
  on an owning LOCAL (rc 139 vs a `use of moved variable` rejection). None runs through these
  collectors; they all lose the operand's `NamedVariable` provenance (`CallerName` and its borrow
  origin) through the parenthesized primary. **A text peel fixes the arms that compare SPELLINGS; it
  can do nothing for the arms that need the resolved variable.** An `EmitReturnExpression` peel was
  written, measured to change nothing observable (`return (localOwner)` was already rc 0 both ways,
  and the borrowed-parameter case is gated off by `IsBorrowedStructParameter` before the name is
  used), and REVERTED rather than shipped unverified.
  Review then found the axis the round had not enumerated: parentheses are not the only TEXT
  WRAPPER the semantic consume arms see through. A redundant same-type cast `(UBox)p` and the
  `as` form `p as UBox` spell `"(UBox)p"` / `"pasUBox"`, match no parameter, and reproduce the
  original rc 134 exactly - identical on both binaries, filed as
  `p1/cast-wrapped-consume-source-defeats-owning-sink-inference.md`. **When a fix normalizes a
  SPELLING before comparing it to a name, enumerate every wrapper that changes the spelling
  without changing the value, not just the one in the issue title.**
- **The 2026-08-09 lambda owning-sink SITE lesson (`fix/lamsink`)**: `ApplyOwningSinkInference` ran
  only on a `FunctionDefinitionContext`, so a LAMBDA literal's parameter list never got it while
  the callee-side consume arms (brace element, bare-brace declarator, slot store, decl init, and
  even an unconditional `move p`) were already shared with functions - callee freed, caller freed,
  rc 133 in all six spellings. This is the same two-halves shape as the `fix/parmbrace` entry
  above, one level out: the callee half was complete and the CALLER half had no door at all.
  The landed shape: inference runs over the lambda's own params against `ctx->lambdaBody()`; the
  result rides the funcptr TYPE as `FuncPtrParam::IsOwningSink` / `IsConsumeInferredSink` (both on
  the `--init` round-trip inside `fpp`, and on the C-header `TvToJson` pair); the indirect call
  site delegates to `ApplyMoveParamTransfer` through a synthesized param list that leaves `IsMove`
  false, so a program with no inferred sink stays bit-identical on the old path. A funcptr bound
  to a NAMED function carries that function's inferred sinks too (`FuncPtrSigOfSymbol` /
  `MakeFuncPtrTypeAndValue`), which is what fixed `function<void(UBox)> f = sinkfn; f(a);`.
  **A declared type cannot SPELL an inferred fact, so it has to be ADOPTED, and adoption has
  doors.** Only ONE was closed - the declarator initializer, next to the pre-existing per-param
  `IsMove` agreement check. The `IsMove` check's own polarity is the wrong model to copy here:
  REJECTING on disagreement would refuse the filed repro, because the LHS spelling can never state
  the flag. Adoption is a UNION that never clears, because over-approximating a sink leaks at worst
  while missing one double-frees. The remaining doors (closure-typed parameter, `return` through a
  declared closure return type, closure-typed struct field, plain `=` rebind) are filed as
  `p1/inferred-owning-sink-is-lost-when-a-closure-crosses-a-declared-boundary.md`; the parameter
  and return doors probably cannot be closed this way at all, since the fact does not travel in the
  closure fat struct.
- **A by-NAME re-lookup of a function symbol returns the FIRST non-method overload, so adopting
  per-parameter facts from it is declaration-order dependent - `GetFunctionForFuncPtr` is the
  shape-matched twin and the only safe source.** The first cut of the above read the initializer's
  sinks with `MakeFuncPtrTypeAndValue(assignmentExpression->getText())`, which walks straight to
  `functionTable.find(name)->second.front()`. With `ov(UBox)` (borrowing) and `ov(VBox)`
  (consuming) declared same-arity, that produced BOTH failure directions from one bug: with the
  consuming overload first, `function<void(UBox)> f = ov; f(a);` became a hard "use of moved
  variable" on a program the base runs correctly, and with the borrowing one first the consuming
  bind silently kept its rc 133 double free. The suite was green and the fix's own 38-file corpus
  was green with both in it - no cell declared two same-arity overloads, so nothing could see it.
  The fix is to adopt from the overload the surrounding code ALREADY resolved and is about to
  call (`FuncPtrSigOfBoundFunction(name, boundFn)` keyed on the returned `llvm::Function*`), which
  makes it structurally impossible for the adopted facts and the called body to disagree. Frozen as
  legs 17-19 of `testLambdaParamOwningSink`, with the two overload pairs declared in OPPOSITE
  orders so neither accept leg can pass by being the first-registered one.

- **`fix/bvfield` - one ruling for six consume arms, and an ORACLE with two holes in it.**
  Consuming a field of a BORROWED by-value struct parameter (`UBox o = w.b;` inside
  `int f(Wrap w)`) zeroed only the callee's bit copy while the caller's `Wrap` still owned and
  freed the same `Res` - rc 133 in every implicit store spelling, while the explicit `move w.b`
  had rejected it since it was written. The fix routes all six `ClassifyOwningAssignSource` consume
  arms - local/global destination, fixed-array element, deref destination, container element slot,
  the brace/array slot emitter, and the decl-initializer - through one
  `RejectConsumeOfBorrowedByValueParamField` placed immediately after the classify and gated on
  `kind == Move`. That gate IS the polarity: a COPYABLE owner classifies Copy and never reaches the
  guard, which is why `OaiCopyWrap`, `OaiPodWrap` and `OaiSWrap` field consumes stayed green with
  no exclusion written for them.
  **Following the issue file's "use the same test the explicit spelling uses" instruction verbatim
  would have shipped that test's own two holes.** Running the oracle against itself first - the
  `internal/skill/fix-issue` oracle caution, applied - found `move w.a.b` (NESTED path) and
  `move (w.b)` (parenthesized) both compiling and double-freeing on the base commit. The nested one
  was a missing INPUT, not a missing check: `TypeAndValue.ParentVariableName` names only the
  IMMEDIATE parent, which on `w.a.b` is the field `a`, so `IsBorrowedStructParameter` was being
  asked about a field name. A new `NamedVariable::FieldPathRoot`, set once at the field-access site
  from the parent's own root, fixed the implicit AND the explicit spelling together. The
  parenthesized one is provenance loss upstream and stays open as a cell of
  `p1/parenthesized-operand-loses-named-variable-provenance.md`.
  **Two more spellings looked like this bug and were proved NOT to be, by a control with a LOCAL
  source.** `return w.b` (rc 133) and `v[0] = w.b` into a `T[]` VIEW (rc 133) both reproduce with
  no parameter anywhere - `Wrap w2; return w2.b;` and `v[0] = w2.b;` abort identically - so neither
  is a borrowed-parameter fault: the return path has no consume arm at all, and the view-element
  destination has none for an INDIRECT source. Filed as
  `p1/return-of-an-owning-struct-field-copies-instead-of-consuming.md` and as a re-measurement that
  promoted `array-view-element-store-orphans-the-old-element` from a p2 LEAK to a p1 double free.
  **The one-line control is what separates "my area" from "the neighbouring bug", and it is cheaper
  than either fixing or excusing the cell.**
  The POINTER-parameter twin (`p2/implicit-consume-of-a-borrowed-parameters-field-has-no-diagnostic`)
  was deliberately left open: `bps_ptrfieldsrc_*` pins it GREEN, so extending the guard there would
  hard-error a program master runs correctly. **"Both spellings want the same single ruling" in an
  issue file is a design preference; when one of them is pinned green, applying it is a maintainer
  decision, not a fix.**

  **Round 2 (review) found the axis the round had not enumerated: SHADOWING, and the defect was in
  the predicate the issue file told the fix to reuse.** `IsBorrowedStructParameter` asked
  `IsFunctionParameter(name)` and then resolved the binding with `GetScopedLocalOrArgument(name)` -
  two lookups that DISAGREE when an inner block declares a local of the parameter's name. The name
  says parameter; the binding is the local, which carries none of the parameter's borrow contract.
  `int f(Wrap w) { { Wrap w; w.b = umk(4); UBox o = w.b; } }` is rc 0 with two allocations and two
  frees on the base commit and became a hard error - a false rejection of a correct program, in the
  most ordinary shape there is. The same defect was ALREADY LIVE in the explicit `move` path it was
  copied from (`scratch/rev_24`), which false-rejects on the base commit too. **Reusing a predicate
  inherits its bugs as well as its answer - the oracle caution applies to the predicate, not only to
  the behaviour.** The fix is the standing rule from "On the code": resolve the name ONCE where the
  scope that gives it meaning is current, and RECORD the answer -
  `NamedVariable::RootIsBorrowedByValueParam`, settled at the field-access site against the resolved
  parent binding, plus a STORAGE-identity check inside `IsBorrowedStructParameter` itself so its
  remaining name-based callers stop being name-only. That second half repaired the pre-existing
  `move` false rejection in the same change.
  Two smaller review findings worth keeping. The reject message advised "or 'alias w' to borrow
  explicitly", and `alias Wrap w` is rc 133 on the base commit - the same double free - so the
  REMEDY WAS A LIE while the rejection it decorated was correct; the clause was dropped from all
  three messages rather than the guard being loosened. And a cosmetic request to name the ELEMENT on
  `w.arr[0]` was attempted, measured (`IsElementAccess` is not set on that source), and REVERTED
  rather than plumbed - the leg was re-pinned to the wording the compiler actually emits. **A
  cosmetic finding that turns out to need new plumbing stops being cheap, and the honest move is to
  pin the real behaviour, not to grow the diff.**
