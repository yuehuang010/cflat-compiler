# Interface issue queue

The tracker for the interface-related entries in `internal/issue/`. Two files already
linked `[[interface-issue-queue]]` before this existed; this is that file.

Not a separate issue - an index. Each row points at the file that owns the detail.
When an issue is fixed its file is deleted (the repo convention), so delete its row
here in the same change.

Last updated 2026-07-29.

## HISTORICAL - attempt 4, now LANDED as `2bcc5a0`

Kept for the design record only. The account below was written while the attempt was held;
everything in it shipped, with two changes made during review - the `llvm.mem*` fix listed as
"still owed" item 1, and the null-store knob (item 3) resolved to `true`, not `false`. See the
"Closed in the 2026-07-29 session" entry for what actually landed and why the knob flipped.

<details>
<summary>Original in-flight notes</summary>

## IN FLIGHT - attempt 4 at the queue head, DELIBERATELY HELD

Status as of 2026-07-29: **paused by the maintainer until the API tier recovers.** Do not
restart this from scratch - the design work and the test infrastructure are done and are the
expensive part. Pick up from here.

**The design (new, and materially different from attempts 1-3).** Consulted Fable, then
verified its load-bearing claims directly against the code. The move that makes it different:
**it never asks reachability.** Attempts 1-3 all tried to answer "which store REACHES this
return", which is unanswerable soundly at emission time. Attempt 4 defers the check to a point
where the function's CFG is COMPLETE, then asks a purely EXISTENTIAL question over the
complete use-list of the returned local's alloca: "does any writer of this slot exist that is
not a frame box?" There is no notion of "reaches", so neither killing failure mode can recur -
the loop case (attempt 3) cannot, because all stores exist by then; the
`if (c>0) else if (c<=0)` case cannot, because the rule never asks whether the non-frame store
reaches the return, only that it EXISTS.

Rule: enumerate every user of the slot. Reject iff at least one store is a ledger-confirmed
`Source == FrameStorage` box AND there is ZERO accept evidence. Loads and dbg/lifetime
intrinsics are neutral; a null/zero store is neutral (the one deliberate accept-bias knob, to
be kept behind a single named switch); **every other user whatsoever - a store with no ledger
record, a `Heap`/`Parameter`/`Global`/`Unknown` record, a call argument, an address escape, a
memcpy, anything unrecognised - ACCEPTS and stops the walk.**

Soundness argument: rejection requires positive whitelisted evidence for every writer and
escape. Therefore EVERY class of missing information - an unseen shape, an unrouted boxing
site, an unclassifiable store, ledger incompleteness from
[[interface-boxing-guards-are-binding-dependent]] - lands on ACCEPT. Only a positive
misclassification could flip it, and see the `FrameStorage` row below for why that cannot
happen.

**Facts verified by hand (do NOT re-derive these):**

| Claim | Verified |
|---|---|
| Post-emission hook with a working twin | `RunNullDerefDataflow`, called at `MainListener.h:7574`; CFG complete there |
| Abort path already discards partial CFGs | `MainListener.h:7537` (`DiscardNullDerefEvents`) |
| Ledger alive at that hook, per-function | cleared `LLVMBackend.h:3462`; parked/restored 8991 / 9018 |
| **Viability probe** - the fat value is ONE aggregate store, not field-wise GEP stores, so the whitelist can see it | `CreateAssignment` (`LLVMBackend.h:13742`) returns a single `llvm::StoreInst*` |
| No zero-init of locals, so an initialised interface local has no spurious null store (this would have made the pass vacuous) | no memset/zero store in `CreateLocalVariable` (`LLVMBackend.h:13591`) |
| **`FrameStorage` cannot be over-stamped** - the one thing that could flip the rule toward rejection | `ClassifyInterfaceBoxSource` stamps it only when the dataPtr, after stripping casts and GEPs, IS an `AllocaInst`; `Square* p = new Square()` arrives as a LOAD of p's slot, so it classifies `Heap`. A true statement about storage, not an inference. |
| The slot's address cannot escape via source | `IShape*` is rejected by the front end ("pointer '*' is not allowed on interface type") - whitelist escapes anyway, synthesized/lambda paths may still do it |
| The return path already resolves the slot | `retStorage`, `MainListener.h:5870-5872` |

Rejected alternative, recorded so it is not retried: a SOURCE-level "tainted binding" property.
It requires observing every assignment site to interface locals, so a missed site produces a
FALSE REJECTION - the wrong polarity, and this family's documented disease is exactly that
assignment sites drift. Ground the rule in the finished IR's use-list instead, where
completeness is a property of LLVM's def-use graph rather than of the compiler having
remembered to log something.

**Artifacts in place:**

- Worktree `../cflat-fix-return-dangle-4`, branch `fix/return-dangle-existential` (off
  `888455e`). The full written spec - anchors, rule, test list, process rules - is at
  `scratch/SPEC.md` in that worktree. **Read it before resuming.**
- Positive corpus: `scratch/rev4/positive/`, 21 files, **21/21 green on master**, with a
  `run.sh` that resolves its own directory so any worktree can run it as
  `run.sh <path-to-cflat>`. Covers the prior attempts' killing shapes PLUS the boxing SOURCES
  the in-tree corpus does not (fat parameter, global, `(new Square()) as IShape`, owning
  pointer local, call result, `?:` join of two heap arms, copy chain, `return move r;`).
- Implementation: **WRITTEN, UNCOMMITTED, PARTIALLY REVIEWED.** 182 insertions across
  `cflat/LLVMBackend.h` (the pass + the pending-record store), `cflat/LLVMBackend.cpp`
  (`ResetForReanalysis` hygiene), `cflat/MainListener.h` (record site + discard + hook), and
  two new scoped-block legs in the EXISTING `Test/errors/err_return_interface_value.cb`.
  Three opus attempts died on transient 529s before their first tool round; the delivered work
  is from a sonnet run against the spec, with the main session owning soundness.

**Verified by hand in the main session (not taken from the agent's report):**

| Check | Result |
|---|---|
| Suite on the NEW binary | 522 passed / 0 failed / 8 skipped |
| Suite on MASTER, re-run to establish the true baseline | **also 522** - so no regression, and the queue's earlier "520" figure was STALE. Corrected. |
| Positive corpus (21 files) on the new binary | 21 / 0 |
| Regression test is NON-VACUOUS | new binary exit 0; **master exit 1** with "FAIL: expected error ... did not occur". A real tripwire, not a test that passes either way. |
| `Test/test_interface.cb` (the 33 `dangle*` functions) | compiles, runs, 56/56, untouched |
| Suite count cannot confirm the new test fires | correct - the legs went into an EXISTING file, so the count is unchanged by design. That is why the master-exit-1 check above was needed. |

**CONFIRMED DEFECT found in the main session's own read - fix this FIRST on resume.** The pass
treats any call user satisfying `CallIsPointerOpaqueIntrinsic` as NEUTRAL, and that predicate
(`LLVMBackend.h:2734`) covers `llvm.dbg.*`, `llvm.lifetime.*` **and `llvm.mem*`**. A
`llvm.memcpy` whose DESTINATION is the slot is a real write of a possibly-non-frame value, so
treating it as neutral can produce a FALSE REJECTION - the exact failure class that killed
attempts 1-3. Two things make this a textbook instance of a hazard already in this file's
working notes:

- The helper's contract is about a POINTER VALUE's escape ("touch the POINTEE, never the
  pointer value itself"), which is sound for its original caller at `LLVMBackend.h:2605` - and
  that caller correctly special-cases `llvm.mem*`. The new pass asks a DIFFERENT question,
  about writes to a SLOT, and for that question touching the pointee IS the write. Reusing the
  helper silently changed its meaning. (Working note: "when an agent cites a justification,
  check it still holds AFTER the change it is justifying.")
- The codebase already knew: the sibling `AllocaIsLoadStoreOnly` comments that for a slot
  "Only debug/lifetime markers are inert here: llvm.mem* would copy the parked" pointer.

Severity: **LATENT, not demonstrated.** Only 4 `CreateMemCpy` sites exist, and the array-element
seeding one (`MainListener.h:8308`) writes through a GEP, which the catch-all already accepts -
so no reachable false rejection was found today. It must still be fixed, because the invariant
the WHOLE design rests on is "any unrecognized user accepts", and this is a hole in exactly that
invariant. Fix is one line and strictly accept-biased: in the pass, treat only `llvm.dbg.*` and
`llvm.lifetime.*` as neutral and let everything else, `llvm.mem*` included, fall to accept. Do
NOT change the shared helper - its other caller depends on the current contract.

Sequencing note: master's working tree was deliberately left pristine (only `.md` edits) because
`x64/Release/cflat` is the reference binary the corpus and the review both compare against.
Fable's zero-risk recommendation to extend `LogInterfaceReturnDangle`'s wording with "binding the
value to a local first does not extend its lifetime" - so the direct diagnostic stops teaching
the laundering workaround - is therefore NOT yet applied. It is worth shipping on its own even
if the pass never lands, and is the honest fallback if attempt 4 also fails.

**Still owed when work resumes, in this order:**

1. The one-line `llvm.mem*` fix above.
2. The ADVERSARIAL review, which has NOT run - the checks above are the main session's own read
   plus mechanical verification, not an independent adversarial pass. Its explicit hunt is a
   legal program turned red. Every round of this work has found a confirmed defect, and this
   round already found one before the review even started; do not skip it.
3. Decide the `kNullStoreIsAcceptEvidence` knob (`LLVMBackend.h`, currently `false` = null store
   is neutral). Consequence of `false`: `IShape r; if (c) r = loc as IShape; return r;` is
   rejected. That is a true conditional dangle and parity with the direct guard, but it is the
   deliberate accept-bias knob and review should weigh it explicitly rather than inherit it.
4. Only then consider whether `FrameLocalDataOfFatValue` can be reduced to a fallback, as the
   fix direction suggests. Not part of this change.

</details>

## Resume point

## IN FLIGHT - generic-interface registration, UNCOMMITTED in the main checkout

Status as of 2026-07-29, end of session. `[[generic-interface-registered-as-opaque-struct]]`
is **fixed and verified but NOT committed** - the working tree holds the compiler change plus
the merged tests. Nothing was committed (repo rule). Do not restart this; read the issue file
and this entry first.

**Where it stands.** ~600 insertions across `cflat/MainListener.h`, `cflat/LLVMBackend.h`,
`cflat/LLVMBackend.cpp`. Verified on macOS:

| Artifact | State |
|---|---|
| `./test.sh Release` | 530 passed / 0 failed / 8 skipped (was 522 at session start) |
| `Test/test_interface.cb` | 85/85 (was 56/56) |
| `scratch/gi/test_generic_interface.cb` | 29 legs, green cold AND `--init`-warm; master fails it |
| `Test/errors/` | 4 new `err_*` legs |
| `Test/library/` | `gi_collide_iface.cb`, `gi_collide_struct.cb` - needed by the cross-file collision legs, NOT strays |

What now works that did not: generic interface as parameter, struct field, return type, generic
type argument, with multiple/nested/pointer type arguments, across files, and under `if const`.

**The final cleanup round is DONE and verified** (six review rounds plus this one). It fixed four
accuracy/dead-code/test-vacuity items, no miscompiles:

- **`certain` had TWO causes and they were conflated.** `expect_error` blocks also set
  `certain=false`, so they wrongly got the `if const` hint - the false-cause class by another
  door. Now split: `certain` gates the struct-name veto; a separate `ifConstUnfoldable` is the
  ONLY thing that may populate `ifConstUncertainInterfaceNames`. Keep them distinct.
- **"Reports every offender" was false** - `LogError` never returns (`exit(1)` on the CLI path),
  so the loop ran one body and its dedupe set and RAII restore guard were dead. Now ONE aggregated
  diagnostic: caret on the first offender, the rest named in the body. Nothing runs after
  `LogError`; do not add code that assumes it does.
- **Aggregation made the `is`/`as`-source records LIVE reporters**, so they were kept, not deleted -
  and both negative tests now pin their ROLE (`"as the source of 'is'"`,
  `"as the target of an interface conversion"`) with the declaration and the operation on separate
  lines so column-sort cannot mask the intended record. Both pins were MUTATION-TESTED: delete the
  record, the test fails; restore it, it passes. Do that for any test pinning a multi-site message.
- Duplicated comment fragment repaired; `RecordInterfaceMaterialization` early-outs on an empty
  instance set; the sort comment no longer claims "source order" (the key is the RECORDING site, so
  several fields of one template share a line and order is layout order).

`internal/testing-notes.md` gained a section on the scoped-`expect_error`/deferred-diagnostic trap.

**The design, and why it is the fourth shape tried.** Four strategies failed first; the surviving
one is **record-then-resolve**:

- `RecordInterfaceMaterialization(name, role)` appends `{name, file, line, col, role}` to
  `gts.materializedInterfaceUses` at eight value-materialisation sites (global, local, struct
  field, by-value parameter slot, rebox source, rebox target, argument coercion, `is`/`as`
  source). It **cannot reject**, so a missed site degrades to "no diagnostic" - never to a false
  rejection or a false cause.
- `ResolveMaterializedInterfaceUses()` runs once where `interfaceTable` is COMPLETE (in `Compile`
  after `CheckPoisonedFunctionCalls`, and in `Analyze`).

**Why the earlier shapes failed - do not retry them:**

1. *Reject at end-of-compile over every syntactic occurrence.* False-rejected mainstream code:
   `int countOf<T>(IEnumerable<T> e)` and any `if const (__WINDOWS__)`-guarded helper with a
   generic-interface parameter. The set is filled from every occurrence including uninstantiated
   template bodies whose recorded name is the placeholder `IEnumerable__T`, which can never gain
   an `interfaceTable` entry.
2. *Reject at each materialisation site.* Site enumeration failed twice running - rebox, then
   local, then field, then **global**, then **by-value parameter** - each miss a SIGSEGV.
3. *Delete the check entirely.* Re-opened a vtable-laundering miscompile (below).
4. **The killer argument against any at-site check**: the precondition "in
   `genericInterfaceInstances`, not in `interfaceTable`" is **legitimately transient**. The
   codebase says so at `LLVMBackend.h:16301` - a generic interface lowers to a fat pointer before
   its table entry exists, because the forward-ref scan materializes signatures first. So
   `CreateStructType`'s field loop cannot distinguish "never instantiated" from "not yet", which
   is why it emitted a false cause. Deferring did not merely fix the message - it turned three
   legitimate shapes (`class Wrapper<U> { Container<U> ci; }`, an interface declared after the
   struct using it) from REJECTED into WORKING.

This is the same lesson as `2bcc5a0`: **defer the decision to where the facts are complete, but
capture location and role where you have them.**

**Six review rounds, six confirmed defect sets, EVERY ONE while the suite was green** (522, then
526, then 530). In order: (1) cross-file struct/interface name collision false-rejected a legal
generic struct - reachable from `core/interfaces.cb` via any transitive import, so a user
`struct IQueue<T>` broke; (2) a generic interface in a dead `if const` branch compiled and
SIGSEGV'd; (3) the round-2 fix's `if const` decider drift turned `if const ((__MACOS__))` -
merely parenthesized, and idiomatic in `core/cruntime.cb:63` - into a raw verifier failure, plus
the mainstream false rejections above; (4) **vtable laundering** - an unrouted name is not called
but ASSIGNED THROUGH, so `IA ia = a; GiU<int> u = ia; IB ib = u; ib.M7()` dispatched `IB::M7`
through a 1-slot `IA` vtable (exit 133, and a silent wrong-value variant at exit 0); (5) global
and by-value-parameter materialisations still SIGSEGV'd, `--check` reported the crashing program
CLEAN, and the `is`/`as` backstops were **dead code** because `ClassifyCastSource` returned
`InterfaceValue` without populating `shape.TypeName`; (6) the four items above.

**Generalizable lessons, both already in this file's themes:**

- **When you widen a ROUTING predicate, every VALIDATION predicate keyed on the narrow one
  becomes a hole.** `GetType`'s `isInterface` was widened to accept `genericInterfaceInstances`;
  `IsInterfaceType` (which reads `interfaceTable` only) was not - and `ReboxInterfaceIfNeeded`'s
  `if (!IsInterfaceType(src) || !IsInterfaceType(dst)) return fatVal;` silently skipped the
  conversion guard. Sweep for the other copies; a 68-site sweep was needed here.
- **A test can assert a lie.** Two negative tests passed while unable to reach the leg they
  claimed - once via a local caught by a different site, once because a declaration and a
  conversion shared a line number. Both were found by review, not by the suite.

**The struct-wins tiebreak must allow COEXISTENCE, not pick a winner.**
`Test/test_generics.cb` declares `struct Container<T>` (line 21) AND `interface Container<T>`
(line 204) and is green on master - it emits both `%Container__int` and
`Storage__int_Container__int_vtable`, because the two roles live in different maps and `GetType`
prefers `interfaceTable`. An exclusive decision at pre-declare time is the WRONG SHAPE. Note this
collision is also why the issue file's suggested backstop `LogError` ("a name in both
`dataStructures` and `interfaceTable`") was **deliberately not shipped** - it would false-reject
that green test.

**Before deleting the issue file**, narrow its accept set: several shapes it claims are filed-but-
unfixed (see the new rows in the open tables). Do not close it on a promise it does not keep.

- master is the closure-parameter lowering fix (`df32dd8`), linear, tree clean. The
  primitive-array boxing fix sits on top of it.
- Full verification re-run on macOS with that fix: **522 passed / 0 failed / 8 skipped**,
  examples **35 / 0**. (Baseline at `df32dd8` was 518/0/8; the new error test adds two, cold
  and warm cache.) LSP was NOT re-run - it is Windows-only.
  - This figure was recorded as 520 and was WRONG; `./test.sh Release` on `888455e` measures
    **522** - re-measured 2026-07-29. Trust a fresh run over any number written here.
- **The return-dangle issue is CLOSED** (`2bcc5a0`) and with it the LAST member of the
  `as`/`is` family.
- **`[[generic-interface-registered-as-opaque-struct]]` was the next head and is now FIXED but
  UNCOMMITTED** - see the IN FLIGHT section above. Finish the cleanup round, review the diff,
  commit, then narrow and delete the issue file.
- **The next head is `[[generic-template-namespace-key-space]]`** (consolidated 2026-07-29). It
  merges what were three separate issues - the core-template veto, the namespaced generic interface
  signature, and the declared `c9acb6c` scope limit - into their one root: generic templates have a
  namespace-blind key space, with three sites disagreeing about whether the key is qualified, bare,
  or the spelled base verbatim. Probing it widened the blast radius twice and narrowed the severity
  once, so read the issue before scoping the work:
  - It is **not interface-specific.** A generic STRUCT or CLASS in a namespace is equally unusable,
    qualified (`unknown type 'NS.Box__int'`) or bare-from-inside (`incomplete layout`, a nonsense
    diagnostic mentioning C interop that is not involved). The three predecessor issues all framed
    it as an interface problem because that is what was being looked at.
  - The predecessor's **"SILENT MISCOMPILE" severity did not survive its own repro.** It was filed
    as a repro *direction*, never built. Built this session: differing contracts reject cleanly,
    identical contracts are indistinguishable, and the qualified spelling errors out before the
    collision can matter. Honest severity is false rejection plus a silent name collapse.
  - The core-template veto is **not** fixed by namespace qualification, contrary to what this note
    said before probing: core's `list<T>` and a user's global `interface list<T>` are genuinely the
    same key. It needs the tie-break keyed on the declaring file/module - a separate step, kept in
    the same issue because it is the same key space.
  - **Zero test coverage exists** for namespaced generics anywhere in `Test/` or `cflat/core/`,
    which is how an entire unusable feature went unnoticed.
  `[[interface-boxing-guards-are-binding-dependent]]` remains the largest non-generic gap.
- Artifacts of the ABANDONED attempts 1-3 are now DELETED: branch
  `fix/return-dangle-provenance` (`f39410e`) and its worktree `../cflat-fix-return-dangle`
  are gone (2026-07-29). Its lesson is preserved above and in the
  landed commit message: the ledger answers what a value IS, but which store REACHES the
  return cannot be answered soundly while the function is still being emitted - which is
  precisely why the fix that worked defers to a complete CFG and never asks reachability.
- Note for this whole family: all three failed attempts passed the suite at 512/0/8, because
  no in-repo `.cb` used the shapes involved. **That gap is now CLOSED by `3726a75`** - 33
  `dangle*` functions in `Test/test_interface.cb` (run from
  `testInterfaceReturnDangleCorpus()`, line 3500) cover every shape in the abandoned-attempt
  table, and a false rejection is a compile error, so the suite DOES detect one now. Do not
  repeat the old "a green suite proves nothing" claim unqualified; and if that file stops
  compiling, the analysis is wrong, not the test.
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

- **A return-dangle laundered through an intermediate interface local was accepted** - fixed
  as `2bcc5a0`, closing `interface-return-dangle-defeated-by-intermediate-local` on the FOURTH
  attempt after three abandonments. The whole difference is that it **never asks reachability**.
  The return records the slot; the answer is resolved at the end-of-body hook beside
  `RunNullDerefDataflow`, where the CFG is complete, as an existential question over the slot's
  complete use-list. Rejection requires positive whitelisted evidence for EVERY writer, so
  every class of missing information lands on ACCEPT and cannot produce a false rejection -
  which is what killed attempts 1-3.
  - **The null-store knob resolved to `true` (accept evidence), NOT the `false` the design
    shipped with.** Review found four confirmed false rejections under `false`, and the reason
    is the reusable lesson: a slot that is frame-boxed and then nulled before the return cannot
    dangle, so treating the null store as merely NEUTRAL is the "does a frame box MAY-reach the
    return" question - the exact thing that killed attempt 2 - **re-entering through the back
    door**. The null store IS the CFG edge the rule refuses to look at. Under `true` the rule
    stays purely existential: reject only when EVERY writer is a frame box. The flip is
    provably monotone (the flag is read in one place and only ever sets `accepted = true`), so
    it cannot manufacture a rejection.
  - **Do not reuse a predicate across a change of question.** The pass first used
    `CallIsPointerOpaqueIntrinsic` for its neutral set; that helper also admits `llvm.mem*`,
    which is sound for ITS question (a pointer VALUE's escape) and wrong for this one, where a
    memcpy into the slot is a real write. The codebase already knew - sibling
    `AllocaIsLoadStoreOnly` comments exactly this. Latent, not demonstrated, and fixed anyway
    because the whole design rests on "any unrecognized user accepts".
  - `interfaceBoxRecords_` holds raw `llvm::Value*` and is never retired mid-function. Review
    traced all 9 erasure sites and found the invariant holds today (each is either bracketed by
    `SaveBuilderState`/`RestoreBuilderState`, which clear the ledger, or followed by the
    per-function clear before any query). It is now stated at the declaration, because an
    unbracketed mid-function erasure added later would let a freed `Value*` be recycled into a
    spurious taint - a FALSE REJECTION mechanism.
  - Residue filed as [[return-dangle-missed-when-slot-has-extra-user]]: any extra user of the
    slot (notably a method dispatch through it) is accept evidence, so `r.area()` misses the
    dangle where `measure(r)` catches it. Pre-existing behaviour, and widening the whitelist to
    fix it is the direction that produced the earlier false rejections.
  - Two review rounds, both at opus, ~60 adversarial legal programs on top of the 21-file
    corpus. Round 1 found the blocker; round 2 was clean. The corpus and the spec are preserved
    at `scratch/rev4/` (`positive/`, `review/`, `SPEC.md`) in the main checkout.

- **A primitive-element array boxed into an interface was accepted and miscompiled** -
  fixed, closing `global-primitive-array-boxed-into-interface`. The real mechanism is
  UNREACHABILITY, not a guard that failed to fire: `RejectPointerShapedInterfaceUpcast` sits
  behind a `StructImplementsInterface()` early-out at every boxing site, and `"int"` never
  satisfies that, so the shape guard could only ever see class sources. The plain-assignment
  chain then fell out of its if/else with a raw `ptr` in hand and stored it into the fat
  slot.
  - The GLOBAL vs LOCAL divergence is purely **Constant vs Instruction**. A global array
    operand is an `llvm::Constant`, so IRBuilder folds the bitcast into a ConstantExpr, which
    the module verifier does not subject to the instruction-level bitcast check - it verified
    clean and detonated later in SelectionDAG. A local array decays to a GEP instruction, so
    a real `bitcast` INSTRUCTION is emitted and the verifier rejects it. Same source bug, two
    completely different-looking outcomes.
  - The issue file's runtime description was **wrong for the decl-init spelling**: "exit 139"
    was the COMPILER segfaulting during ISel, not the program. It was **right for the
    brace-init spelling**, which was found later in review - `Holder h = { s = gInt };` links
    clean and the PROGRAM exits 139. Both are in the fix.
  - Four boxing sites needed the guard, and the fourth (`CoerceInitValueToInterface`, shared
    by brace-init and the `<Tag attr=...>` element path) was missed on the first pass for
    exactly the reason the root-cause account names. When a guard is placed after an
    implements check, ASSUME there are more copies of that early-out and go find them.
  - Guard polarity, which is the reusable part: it proves three things before rejecting -
    registered interface target, builtin-primitive source element, provably pointer-shaped
    source - and accepts everything it cannot prove. A 493-file corpus sweep plus ~30 hand
    probes found no false rejection. Parens do NOT defeat it; an `auto` intermediate does,
    and that is filed as [[auto-binding-of-fixed-array-loses-shape]] rather than patched by
    widening the guard.
  - `as` and plain now agree, byte-identical, for the GLOBAL spelling. The LOCAL spelling
    still gets the classifier's generic message - the same documented exception a local
    `Circle*[3]` has, since a decayed GEP has no named binding to describe.

- **Returning a `?:` join of concrete pointers as an interface aborted the compile with a raw
  LLVM verifier dump** - fixed, closing `return-ternary-join-concrete-pointers-not-boxed`. The
  issue file's account was RIGHT as far as it went (the return path had no
  `UpcastTernaryPhiToInterface` call, so a phi of raw `ptr` reached `CreateReturnCall`), but it
  missed two things. First, the plain spelling under a `move` return type did not reach the
  verifier at all - it was a FALSE REJECTION ("returned expression is not owned"), because a phi
  is not a `LoadInst` and so fails `IsOwningValue`. Second, boxing alone is not enough: the
  helper builds its fat value with `BuildInterfaceFatValue` directly and never ledgered an
  `InterfaceBoxRecord`, so `FatValueOwnsHeapBox` could not see the join and the non-`move` heap
  arm silently leaked instead of being rejected - and nothing nulled the arms' owning locals, so
  the callee's scope-exit free ran on the object it had just handed out (correct-looking compile,
  garbage value). Fix: call the helper from the return path BEFORE the ownership and dangle
  checks so all of them inspect the fat pointer exactly as they do for the `as` spelling; ledger
  each arm's box with the ordinary provenance classification; and, only for a `move` return, null
  each OWNING arm's source inside that arm's own block so the untaken arm still runs its normal
  null-guarded free (verified 200 constructions / 200 destructions over 100 alternating calls).
  - Boxing EARLY has a trap that cost one review round: the whole-expression owned-return check is
    gated on `right->getType()->isPointerTy()`, so once the arms are boxed it is skipped
    ENTIRELY. That is what cured the false rejection above - and it equally removed the check for
    arms that are NOT owned, turning `move IW f(int c, W* p, W* q){ return c > 0 ? p : q; }` from
    REJECTED into a compiling double free (exit 134). The check therefore had to move INTO the
    per-arm walk. Its polarity is deliberately the opposite of the whole-expression one: it
    rejects only an arm it can PROVE owns nothing (`IsProvablyNonOwningPointerLoad` - a load whose
    slot is a live binding that declares itself non-owning) and accepts every "cannot tell" shape,
    because `IsOwningValue` answers only a `LoadInst` and reading its `false` as "not owned" is
    precisely what produced the original false rejection. `move` parameters, direct `new` arms and
    move-returning call results were all re-verified as still accepted.
  - The dangle gap applied to this shape too and was deliberately left alone at the time:
    `Square a; Square* p = &a; return c ? p : q;` boxes and compiles, exactly as the
    non-ternary `return p;` did. **Still true after `2bcc5a0`** - that fix keys off a
    ledger-confirmed `FrameStorage` box stored into the returned SLOT, and here the frame
    address arrives through a pointer local, which classifies as a load rather than an alloca.
  - A DIRECT `&local` arm names no class, so it is now rejected by the arm-boxing diagnostic
    rather than the dangle one. A rejection either way; the wording is about the wrong thing.

- **Duplicate constructor signature crashed the compiler with no diagnostic** - fixed,
  closing `duplicate-constructor-signature-hangs-compiler`. The issue file's guess (runaway
  recursion or an unbounded loop) was WRONG. `CreateFunctionDefinition` early-returns an
  already-bodied function BEFORE `createFunctionBlock`, which is the only thing that pushes a
  function scope onto `stackNamedVariable`; `ParseFunctionDefinition` guards for exactly that
  and returns, `ParseConstructorDefinition` did not, so `RegisterThisPointer` indexed
  `stackNamedVariable.back()` on an empty deque. The "corrupted map, huge bogus size" in the
  crash dump was that empty-container read, not stack smashing - which is why duplicate
  METHODS and duplicate free functions never crashed. Fix is a mangled-name duplicate check in
  the forward-ref scanner's constructor pre-declare loop plus the missing guard. Generic class
  templates are not covered by the eager check (the scanner returns early for them); the guard
  is what keeps them from crashing, and they silently drop the second body like methods do.
  - The message's parameter renderer is cosmetically lossy on four shapes - `int*[]` and
    `void*[]` drop `ElemPointer` and print `int[]` / `void[]`, `move B*` prints `B*`, and
    `function<int(int)>` prints the internal `__c_fn_ptr`. Diagnostic text only.
  - The noun is picked by "declares a typeSpecifier", NOT by `declarationSpecifiers() == nullptr`:
    per `CFlat.g4:783` a real ctor may carry `inline`/`static`/`const`/`extern`/`stdcall`, so the
    latter test silently drops the diagnostic on those. Verified - do not "simplify" it back.
    That rule has four exceptions, all message-wording only: `move` IS a `typeSpecifier`
    (`CFlat.g4:323`), and `unique`/`alias`/`bond` are not grammar keywords at all - they parse
    as `genericIdentifier`, also a `typeSpecifier` - so a ctor carrying one of the four reads as
    "member". The duplicate is still caught and the crash still fixed; only the noun is wrong.
    Twelve modifier spellings and fourteen return-type spellings were probed; the forward
    direction (every real return type yields "member") had no counterexample.

- **A function-pointer parameter on an interface method was never lowered, in EITHER
  direction** - fixed, closing `iface-thin-function-param-no-lowering`. The direct call path
  converts a closure fat struct to a bare invoker for a thin `function<>` slot, and widens a
  named function / thin value into a fat struct for a `Lambda<>` slot; `CallInterfaceMethod`
  did neither, so both spellings died in the verifier with no source location and the
  fat-to-thin miss also lost its capture-naming diagnostic. Both conversions are now the
  shared helpers `LowerClosureFatToThinFnPtr` and `WidenBareOrThinToClosureFat`, reached on
  the virtual path through `LowerByValueArg` under guards only virtual dispatch can satisfy;
  the interface argument loop also copies `LambdaCaptureNames`. Direct-path IR proven
  byte-identical across twelve modules.
  - The issue was originally filed and first fixed for the fat-to-thin half ONLY, because the
    regression test used lambda literals - the one shape that half handles. A named function
    or a stored `function<>` value into a `Lambda<>` slot still aborted. If you add a closure
    test here, cover all four source shapes (literal, named function, thin variable, fat
    variable) against BOTH slot flavours - that is what the 19 assertions in
    `testInterfaceFunctionPointerParam` do.
  - The widen must not key off `isPointerTy()`: under opaque pointers every data pointer looks
    like a code pointer, and the first cut of the fix would have put a `void*` in a closure's
    code slot and called it. The guard REJECTS ONLY WHAT IT CAN PROVE IS DATA and widens
    everything else. That polarity is load-bearing: the intermediate allowlist version
    (accept only a named function, an `IsFunctionPointer` value or null) false-rejected a
    legal `io.lam(k > 0 ? a : b)`, because a `?:` join carries none of the three. Read
    [[closure-param-accepts-data-pointer]] before touching this - it records why an allowlist
    cannot work here, that the direct path still has the hole, and that the durable fix is
    frontend-recorded provenance rather than interrogating the `llvm::Value`.

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
  [[iface-slot-replay-blames-wrong-slot]], and
  [[iface-arg-lambda-fnptr-type-not-propagated]]. (The thin-`function<>` parameter entry
  that was also listed here is closed - see the 2026-07-29 session below.)

## Open - crashes and silent miscompiles

| Issue | Severity |
|---|---|
| [[generic-interface-registered-as-opaque-struct]] | LLVM verifier failure + false rejections. `IFace<T>` unusable in most positions. |
| [[interface-boxing-guards-are-binding-dependent]] | Double free (exit 134). Parens or `?:` erase the binding the guards key off. |
| [[return-dangle-missed-when-slot-has-extra-user]] | Missed dangle, no diagnostic. Residue of `2bcc5a0`; pre-existing, and NOT to be fixed by widening the whitelist. |
| [[iface-call-does-no-argument-type-matching]] | Silent miscompile then SIGBUS. An `int` reaches a closure slot; the direct path rejects it. |
| [[function-array-body-silently-truncated]] | Silent miscompile, exit 133. NOT interface-related; filed here because it has no other queue. |
| [[nondeterministic-ir-switch-case-order]] | No miscompile - a METHODOLOGY hazard. NOT interface-related; listed here for the same reason as the row above: it is the only index. Read it before using "0 IR diffs" as proof. |
| [[closure-param-accepts-data-pointer]] | SIGSEGV, no diagnostic. DIRECT-path residue; the virtual path is now guarded. |
| [[auto-binding-of-fixed-array-loses-shape]] | Silent miscompile (`auto` on an array is not indexable), and it defeats the primitive-array guard. Fix the deduction, NOT the guard. |
| [[null-coalesce-join-into-interface-not-boxed]] | Verifier failure, no diagnostic. A THIRD join shape - not the `?:` double-free entry, not the return-path entry. |
| [[fixed-array-copy-invalid-bitcast]] | Verifier failure, no diagnostic. NOT interface-related; filed here because it has no other queue. |
| [[fixed-array-parameter-not-callable]] | False rejection: a `T[N]` parameter registers as a bare `T`, so no call resolves. NOT interface-related; same reason as the row above. |
| [[interface-method-call-on-null-value-segfaults]] | SIGSEGV (139), no guard. Fires on a PLAIN non-generic interface on MASTER too - `NgLive lv = default; lv.Get();`. Pre-existing and language-wide, not generic-specific. |
| [[ifconst-const-global-condition-corrupts-ir]] | Missing block terminator in an unrelated already-emitted function. `if const (<const global>)` at file scope; `DecideIfConstCondition` hard-codes `forceScratch=false`. Identical on master. NOT interface-related. |
| [[interface-type-alias-not-resolved-in-is-as-target]] | Wrong answer + false rejection: `ia is AliasIB` rejected while `ia is IB` works. `IsInterfaceType` resolves aliases; the ~12 direct `interfaceTable.find/count` sites do not. Pre-existing; fix with one resolving accessor. |

## Open - false rejections and accept-set problems

| Issue | Severity |
|---|---|
| [[bare-interface-name-resolves-outward-before-namespace]] | Makes the documented namespace workaround awkward. |
| [[iface-ifconst-base-clause-implementor]] | Implementor inside a non-taken `if const` -> "no class implements it". |
| [[unique-array-view-accepted-as-generic-type-argument]] | Inconsistent accept set, no miscompile shown. |
| [[generic-template-namespace-key-space]] | **Next head.** A whole feature is unusable: ANY generic template declared in a namespace - struct, class or interface - cannot be named, qualified or bare. Also silently reverts to the UNFIXED compiler for 18 core generic names (`list`, `tuple`, `channel`, `span`, `array`, `stack`, `queue`, `dictionary`, `Pair`, `view`, ...). Consolidates three previously-separate issues; zero test coverage today. |
| [[function-type-as-generic-interface-type-argument]] | `C<function<int(int)>>` fails on both binaries. Clean failure, no verifier issue. |
| [[sizeof-of-generic-instantiation]] | `sizeof(B<int>)` -> `unknown type`. NOT interface-related - fails on a plain generic struct too; `sizeof`'s operand skips the generic mangling/queue path. Check `alignof` and cast operands in the same pass. |
| [[generic-interface-cannot-inherit-generic-interface]] | False rejection: `unknown parent interface` when a generic interface's base clause names another generic interface. Fires on INSTANTIATION, not on the declaration - declaring and never using it passes. |
| [[duplicate-generic-template-name-silently-accepted]] | No miscompile shown. Undocumented "struct wins" tiebreak lets a generic struct and generic interface share a name with no diagnostic. |

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
- **The generic-interface work is the strongest datum: SIX rounds, SIX confirmed defect sets,
  every one found while `./test.sh` was GREEN** (522, 526, 530). Among them a SIGSEGV, a silent
  wrong-value miscompile, a stdlib-breaking false rejection, and two checks that were dead code.
  A green suite is a floor here, never the bar.
- **Scope each review round to what the last round CHANGED, and say what not to re-verify.**
  Rounds that re-covered settled ground burned budget; the rounds that found the worst defects
  (laundering, dead backstops) were the narrowly-scoped ones. Listing the previous round's
  confirmed-clean items in the brief is what buys that focus.
- **A "safe with listed fixes" verdict is not "clean".** Read the list; twice in this work the
  listed items included a factually false diagnostic and a test that could not reach its leg.

**On the issue files themselves**

- **Probe an issue before scoping work from it, even a carefully written one.** Consolidating three
  files into `[[generic-template-namespace-key-space]]` took under an hour of probes and corrected
  all three: one claimed a severity its repro does not support, one asserted a shape works that does
  not, and my own queue note claimed two issues shared a fix that they do not. A filed root cause is
  a hypothesis with a citation, not a measurement.
- **A severity recorded from a repro DIRECTION is unverified.** Both "silent miscompile" claims in
  this queue that were never actually run turned out to be false rejections instead. Mark the
  difference in the file so the next reader does not budget for a wrong-value hunt.
- **Issues filed while looking at feature X tend to be described as X bugs.** Three separate files
  called this an interface problem; it affects every generic template kind. Before fixing, probe the
  neighbouring kinds - it is cheap and it sets the real scope.
- **Consolidate on the shared ROOT, not the shared symptom, and say what you did NOT merge.** The
  adjacent files here (`[[duplicate-generic-template-name-silently-accepted]]`,
  `[[bare-interface-name-resolves-outward-before-namespace]]`, `[[iface-namespace-follow-ups]]`)
  overlap partly; merging them would have buried unrelated findings, so they are cross-linked with
  the specific findings named.

**On changing approach vs. patching**

- **When site enumeration misses twice, change the method - do not add two more sites.** The
  generic-interface fix cycled reject-at-end-of-compile -> reject-at-site -> delete-the-check
  before landing on record-then-resolve. Recording cannot reject, so a MISSED site degrades to
  "no diagnostic" instead of to a false rejection; resolving only where the facts are complete
  removes the transient-state ambiguity entirely.
- **Check whether your precondition is TRANSIENT before rejecting on it.** "In
  `genericInterfaceInstances`, not in `interfaceTable`" reads like a bug state and is the normal
  state during monomorphization (`LLVMBackend.h:16301` says so in a comment). Deferring turned
  three legitimate shapes from rejected into working - the check was not just mis-worded.
- **When you widen a ROUTING predicate, every VALIDATION predicate keyed on the narrow one
  becomes a hole.** `GetType`'s `isInterface` was widened; `IsInterfaceType` was not; a
  `if (!IsInterfaceType(src) || !IsInterfaceType(dst)) return fatVal;` early-out then skipped
  the conversion guard and laundered a 1-slot vtable into an 8-slot interface. A 68-site sweep
  was needed to be sure. Same shape as the "more copies of that early-out" note below.
- **A deleted safety check needs its harm argument tested, not reasoned.** "A fat pointer with
  no vtable must crash at the first method call" was true of the CALL path and false as a general
  claim - the unrouted type is a PIPE you assign through, and the call lands on a routed
  interface whose lookup succeeds. One experiment settled what three paragraphs of reasoning
  could not.
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
- **A test can PASS while unable to reach the leg it claims.** Two generic-interface negative
  tests did exactly that - one pinned a shape caught by an EARLIER record site, the other passed
  only because a declaration and a conversion happened to share a line number. Both were caught
  by review, never by the suite. When a test pins a diagnostic that several sites can emit, pin
  the ROLE/site-specific wording, not the shared prefix - and prove which site fired.
- **A deferred (end-of-compile) diagnostic cannot be caught by a SCOPED `expect_error` block.**
  The block closes first and prints `FAIL: expected error ... did not occur`, then `exit(1)`s
  BEFORE the real diagnostic - so the stated reason is the opposite of the truth. Use the bare
  file-scope `expect_error` form for anything deferred, and say so in the file.
- **Verify a negative test is non-vacuous against the RIGHT baseline.** A test can be vacuous vs
  master yet still be a real tripwire for a defect introduced mid-work: the dead-`if const` leg
  passes on both master and the fixed binary, but would have caught round 2's compile-then-
  SIGSEGV. Record which binary it discriminates against, rather than labelling it vacuous.

**On process**

- Issue files can be wrong: one repro did not reproduce as written, another described a
  fault milder than reality. Have the fix agent verify the repro FIRST and report what it
  actually saw.
- When an agent proposes diverging from the issue file's fix direction, treat it as a RISK.
- Tell agents to use repo-root `scratch/` and never run `git stash`.
