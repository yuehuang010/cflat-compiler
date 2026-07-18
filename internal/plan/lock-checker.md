# Lock checker (contention profiler + static lock-order checker) - DESIGN

Status: DESIGN ONLY. No code written. This captures the design and grounds every
hook point in the current codebase. Nothing here is finalized; open questions at the end.

## Goal

Exploit the fact that CFlat's `lock` clause NAMES the mutex/guardian it protects
(unlike an opaque `pthread_mutex_t` address) to build lock tooling the compiler can
drive from source, not from runtime address inference:

- (A) a RUNTIME contention profiler (wait/hold/contention per lock site and per lock
  class), and
- (B) a STATIC lock-order checker (deadlock-cycle detection at compile time), plus
- (C) a hand-over-hand validator for the planned concurrent B-tree
  (internal/plan/lock-hand-over-hand.md).

A and B share one mechanism: the compiler already resolves a lock target to a
concrete lock TYPE and a canonical guardian TEXT during codegen. B reads that at
compile time; A stamps it into an instrumentation call.

## Reality check on the premise (READ THIS FIRST)

The premise is only PARTLY true, and the difference drives the whole design:

- What is known statically is the lock's concrete TYPE plus the CANONICAL SOURCE
  TEXT of the target, NOT a resolved type+field symbol and NOT any per-object identity.
  - The lock statement resolves the target to a NamedVariable and keeps
    `mutexNV.TypeAndValue.TypeName` (MainListener.h:5549) - so the lock CLASS (concrete
    mutex/rwlock type) IS known.
  - Lock-set membership is a `std::unordered_map<std::string, LockMode> currentLockSet`
    (MainListener.h:2044) keyed on CANONICAL LOCK TEXT: arrow-normalized (`p->m`->`p.m`)
    and mode-suffix-stripped, via ArrowNormalizeLockText / NormalizeLockText /
    LockTextMode (MainListener.h:636-702).
  - A guarded field ties to its guardian by TEXT too: `GuardLockKey` returns
    `ParentVariableName + "." + GuardedBy` (MainListener.h:19297), i.e. source names.
- Consequence: there is NO static per-INSTANCE lock identity. Two different
  `BTreeNode` objects both yield lock text anchored on their source variable name.
  So "per-instance hot spots" and "exact lock identity" are only available to the
  RUNTIME profiler (A), which sees the real mutex address. The STATIC checker (B/C)
  operates at lock-CLASS granularity `(owning struct type, guardian field)` or at
  canonical-text granularity - good enough for order cycles, but a same-class
  self-edge (BTreeNode.mtx while holding BTreeNode.mtx - the canonical hand-over-hand
  shape) cannot be proven safe or unsafe by identity alone. See open questions.
- Raw acquire/release is NOT a dedicated primitive. `m.acquire()` / `m.release()`
  are ordinary methods on a `[Capability(ILockable)]` type (core/mutex.cb:33,39); the
  capability table lists the method names (kCapabilities, MainListener.h ~141-160).
  Today ONLY the scoped `lock(){}` updates currentLockSet; a bare `m.acquire()` call
  does NOT. So C must add held-set tracking for the raw path from scratch.

Net: B and C are structurally sound (all paths, class-level identity known) and still
beat TSan for the order problem, but the plan must talk about lock CLASS + source text,
not "exact per-object identity". A is where true per-instance data comes from.

## Baseline: how `lock` works today

- Grammar: `lockStatement : lockClause compoundStatement`, `lockClause : 'lock' '('
  lockArgList ')'`, `lockArgList : expression (',' expression)*` (CFlat.g4:560-569).
  `lock` is an inline literal soft keyword (same mechanism as `program`). Also
  `lockFieldGroup : lockClause '{' (declaration|functionDefinition)+ '}'`
  (CFlat.g4:810) for guarded field groups, and a `lockClause?` on `functionDefinition`
  (CFlat.g4:783) for RequiredLocks.
- Codegen: handled inline in the hand-written walker `ParseStatement`, NOT an
  ANTLR enter/exit - MainListener.h:5486-5665. For each arg it resolves the target,
  classifies it against the capability table, calls acquire, records an `AcquiredLock`
  { canonical, acquireMethod, releaseMethod, typeName, heldMode, mutexPtr }
  (MainListener.h:5493-5619), then pushes a scope whose `lockCleanup` slot
  (LLVMBackend.h StackState::LockCleanup, single slot today) releases on EVERY exit
  path via EmitDestructorsForScope. The body runs with currentLockSet updated
  (MainListener.h:5637-5645) and restored on exit (5654-5660).
- Identity normalization ("the canonicalization detail"): GetLockArgCanonical
  (MainListener.h:19248) + NormalizeLockText/ArrowNormalizeLockText (636-670).
  `lock(p->mtx)` and `lock(p.mtx)` canonicalize to the same `p.mtx`; `.read/.write/
  .optimistic` suffix is stripped to a LockMode. Both passes must agree (comments at
  MainListener.h:1357,1414).
- Guarded access checks (all TEXTUAL): CheckGuardedWrite (MainListener.h:19308) and
  CheckCallSiteLocks (19335) reconstruct a key string and probe currentLockSet.
- Capability model: `[Capability(ILockable, ...)]` (core/interfaces.cb); mutex and
  rwlock opt in. No lock type is hardcoded (kCapabilities, MainListener.h ~141-160).

## Runtime lock type and lowering

- `mutex` (core/mutex.cb:29): one `void* _srw` slot; `acquire()` -> `os.mutex_lock
  (&_srw)`, `release()` -> `os.mutex_unlock(&_srw)` (mutex.cb:33-42). Non-recursive.
  `rwlock` (core/rwlock.cb) adds shared/exclusive; optimistic path in
  optimistic-lock-coupling.md is a lambda call, not an acquire/release pair.
- os.* chooses the platform representation (SRWLOCK inline vs pthread heap).
- Precedent instrumentation already lives on this path: `if (__fuzz_hook != nullptr)
  { __fuzz_hook(1/2); }` at acquire/release (mutex.cb:34,40) - see tool A.

## Tool A - runtime contention profiler

What it records: at each lock SITE (file,line) and thread id, wait time (acquire
enter->granted), hold time (granted->release), and contention count; aggregated by
lock CLASS (mutex type + guardian) and, using the runtime mutex address, per-INSTANCE
hot spots. Optionally attributes hold time to sub-regions so it can say "hoist this
memcpy out of the lock."

Hook points (grounded):
- Reuse the existing global-function-pointer-hook pattern rather than inventing one.
  `__fuzz_hook` is a `function<void(int)>` global in core/memory.cb:20, installed by
  a diagnostic module's `enable()` (core/diagnostic/thread_fuzz.cb:112) and called
  null-guarded at every sync point. Model A the same way: add a
  `__lock_profile_hook` global + a `core/diagnostic/lock_profile.cb` that installs it,
  so normal builds pay only a null check and pay nothing unless imported.
- The signature must carry identity the fuzz hook lacks: `(int event, void* mutexAddr,
  u32 siteId)` where event = wait-begin / acquired / released. `mutexAddr` gives
  per-instance aggregation at runtime; `siteId` indexes a compiler-emitted table of
  (file,line, mutexTypeName, canonicalText).
- Emission: the lock statement already has everything at MainListener.h:5601-5619
  (mutexPtr = the release address, mutexTypeName, canonical text, source location via
  SetSourceLocation at ctx->getStart()->getLine()/getCharPositionInLine()). Wrap the
  acquire call (5608) with wait-begin/acquired hook calls and the scope teardown
  release with a released hook call. Emit the `released` hook BEFORE the actual OS
  unlock in the teardown, or hold-time bleeds into another thread's wakeup (Fable).
- Caller-site id for the RAW path is NOT worth it in v1 (Fable - decisive). The
  dominant path (scoped `lock(){}`) gets true caller sites for free (instrumented at the
  lock statement). The raw path is the minority (hand-over-hand internals) and mutexAddr
  already gives per-instance hot-spot attribution there - which is what B-tree tuning
  actually needs. Threading a caller id means changing acquire()'s signature or emitting
  a TLS site-id store at every recognized acquire - real surface for marginal report
  quality. Mark raw rows "(raw)" (callee-site granularity); add the TLS-site mechanism
  later ONLY if profiling proves the need. Also CUT sub-region hold-time attribution
  from v1 - it is statement-level instrumentation, a different tool.
- Thread id: `os.thread_current_id()` (core/os.cb:1017 -> GetCurrentThreadId /
  gettid / pthread_threadid_np) - already cross-platform.
- Output: a report printed at process exit (or on demand) - the diagnostic module owns
  a table and a dump function, exactly like thread_fuzz/heap_audit. Columns: lock class,
  site file:line, count, total/avg wait, total/avg hold, max hold, contention ratio.
  NOTE: this is a COMPILED-PROGRAM runtime shim (a core/diagnostic/*.cb module linked
  into the user program), NOT CompilerManager.h - that installs the COMPILER's crash
  handlers and is unrelated.

## Tool B - static lock-order checker (the standout)

What it checks: build a directed lock-order graph with an edge A->B whenever lock B is
acquired while A is held. Any cycle is a potential deadlock, reported at compile time
with both acquisition sites and the lock CLASS identity. Beats TSan structurally:
covers all code paths without running, and knows the lock class from the type system.

Identity for nodes: `(owning-struct-type, guardian-field)` for guarded/field locks
(resolvable from mutexTypeName + GuardedBy + the field's owning struct in
dataStructures), or `(mutexTypeName, canonical-text)` for locals/globals. NOT
per-object (see reality check).

Hook points:
- DO NOT clone ForwardRefScanner for this (Fable review - decisive). A syntactic
  pre-pass CANNOT build the interprocedural edges: a call-site edge requires knowing
  WHICH function was called, and that is overload resolution (receiver type, generic
  monomorphization, interface dispatch). The codebase proves it - CheckCallSiteLocks
  (19335) only works because it runs right after CreateOverloadedFunctionCall in the
  MAIN pass and reads `lastCallRequiredLocks`. A pre-pass would re-implement callee
  resolution or fall back to name-matching, whose failure modes are exactly the bad
  ones: UNSOUND (a name maps to the wrong overload / an un-expanded generic -> missed
  edge -> real cycle passes silently), IMPRECISE (canonical-text nodes without type
  info collapse distinct locks across functions and split identical ones -> spurious
  cycles), and a third tree-walk tripling the "both passes must agree" hazard
  (1357/1414).
- INSTEAD accumulate the graph DURING the existing MainListener codegen walk, where
  currentLockSet, resolved mutexTypeName (5549), canonical text, and lastCallRequiredLocks
  are all already live. On each acquire, add edges from every held lock; on each RESOLVED
  call, add edges from the caller's held set into the callee's RequiredLocks (seed a
  function's held set from its own lockClause/RequiredLocks, CFlat.g4:783). Store the
  graph in LLVMBackend. Gets monomorphized generics for free; cannot disagree with
  codegen about what a lock is; a fraction of the code of a new scanner. The only cost
  (analysis runs with compilation) is irrelevant - the flag implies a full compile anyway.
- Node identity: class `(owning-struct-type, guardian-field)`, but KEEP CANONICAL TEXT
  AS A SUB-IDENTITY under the class node - `lock(a.mtx){ lock(b.mtx) }` vs the reverse
  IS detectable at text granularity within visible scopes; collapsing to class-only
  throws away real same-class inconsistent-order detection. Only the genuinely
  text-indistinguishable same-class case (the coupling pattern) gets special handling
  (see below).
- rwlock MODE-AWARENESS is a v1 REQUIREMENT, not an open question (Fable): shared-vs-
  shared acquisitions do not block, so a mode-blind graph fires false-positive cycles on
  any read-heavy rwlock code. Carry LockMode on every edge; the rule is cheap - a cycle
  is a deadlock only if at least one BLOCKING edge participates per node.
- Reuse the canonicalization helpers (NormalizeLockText etc.) and the capability
  classification already in the lock handler so B and codegen agree on what is a lock.
- After the walk, run cycle detection (Tarjan SCC) over the graph.
- SCOPE B v1 TO SCOPED `lock()` + RequiredLocks edges ONLY (Fable). Defer raw
  `acquire()/release()` recognition and cross-branch/return pairing entirely to Tool C -
  that IS C's held-set/loop-merge problem in disguise; do not solve raw-lock dataflow
  twice. Feed C's held set back into B's graph once C exists.
- Output: LogError at the acquisition site of the back-edge, naming both lock classes
  and both file:line sites (ctx->getStart()->getLine()/getCharPositionInLine()). The
  graph unions edges across ALL paths, so a reported cycle can span mutually-exclusive
  branches - the diagnostic must say "POTENTIAL" deadlock and show both sites; do NOT
  drift toward error-by-default. Gate behind `--check-lock-order` (pairs with `--check`).

Same-class self-edge (BTreeNode.mtx while holding BTreeNode.mtx - the coupling shape):
NEVER silently suppress (that hides the two-instances-inconsistent-order bug forever)
and NEVER hard-error (fires on the flagship B-tree every time -> users disable the flag).
Report-with-caveat as a SEPARATE, lower-severity diagnostic category, DEDUPLICATED to
one report per lock class, with a source annotation (ordered/coupled marker on the lock
field or function) to acknowledge and silence it. That annotation is the forward-compat
slot: when Tool C ships, a C-validated hand-over-hand region auto-suppresses, and the
annotation is the fallback for unvalidated code. Cross-class cycles stay HARD reports.

## Tool C - hand-over-hand validator

Ties directly to internal/plan/lock-hand-over-hand.md (read it - do not duplicate).
That plan already commits to:
- The held lock as a COMPILE-TIME-ONLY, flow-sensitive typestate riding on the POINTER
  VARIABLE (sibling to NamedVariable.IsMoved), transferred on assignment by
  piggybacking the existing move-transfer hook (MainListener.h:8227). No runtime guard
  object, no data projection (maintainer constraint).
- Surface `lock(x.mtx);` / `unlock(x.mtx);` (soft keyword `unlock`) as non-scoped forms
  on top of the still-legal raw `acquire()/release()`.
- Reuse of the use-after-move plumbing: IsMoved/MovedFields, Mark*/MovedUseSubject
  (LLVMBackend.h ~694, 14134-14293), and the if/else Save/Restore/Merge of flow state
  (MainListener.h:5182-5202).
- THE named gap: loops (while/for/do/for..in) and switch do NO snapshot/merge - a
  single linear walk via ParseControlledBody (MainListener.h:4138, called from
  4735/4792/4857/5000/5113). Hand-over-hand lives in a loop. Held-lock state is a
  MUST-analysis, so this needs NO iterate-to-fixpoint: a loop-head snapshot + single
  walk + a per-variable "back-edge state SUPERSET-OR-EQUAL head snapshot" invariant
  check is a fixpoint certificate (one pass suffices). See lock-hand-over-hand.md "The
  one real gap" for the exact edge rules (continue is a back-edge; loop-exit state is
  the intersection of ALL exit edges incl. every break; do/while snapshots at body
  entry). This merge is absent for ALL flow-sensitive state today, not just locks.

What C adds on top of that plan (validation, not just permission):
- acquire-child-before-release-parent: on `unlock(cur.mtx)` require the successor lock
  already in the held typestate (no unprotected gap).
- consistent root->leaf order: reuse B's lock-order graph restricted to the coupled
  walk; the coupling edge parent->child must not contradict the global order.
- no orphaned locks on early-exit/return/break: the held typestate at every exit edge
  must be empty (or explicitly handed off) - the scope-exit auto-release the plan
  proposes is the leak-safety net; C turns a still-held explicit lock on a NON-exit
  merge into a diagnostic.
- the hard part is exactly the loop-merge fixpoint above.

## What exists today vs what is new

| Capability | Today | New work |
|---|---|---|
| Lock target -> concrete TYPE | Yes (MainListener.h:5549) | reuse |
| Lock target -> canonical TEXT identity | Yes (19248, 636-670) | reuse |
| Lock target -> per-OBJECT identity | No (text only) | runtime-only (A, mutexAddr) |
| Held-lock set during scoped lock body | Yes (currentLockSet 2044) | reuse |
| Held-set tracking for raw acquire/release | No | new (B, C) |
| Graph accumulation during codegen walk | Yes (currentLockSet, mutexTypeName, lastCallRequiredLocks all live in main pass) | add edge-recording + Tarjan (NOT a ForwardRefScanner clone - see Tool B) |
| Cross-function required-locks | Yes textual (CheckCallSiteLocks 19335) | extend into graph edges |
| if/else flow-state merge | Yes (5182-5202) | reuse |
| loop/switch flow-state merge | No (linear ParseControlledBody 4138) | new (C, shared with move safety) |
| Global function-pointer runtime hook | Yes (__fuzz_hook memory.cb:20) | clone -> __lock_profile_hook (A) |
| Per-thread id at runtime | Yes (os.thread_current_id os.cb:1017) | reuse (A) |
| Diagnostic shim module pattern | Yes (core/diagnostic/thread_fuzz.cb) | clone -> lock_profile.cb (A) |
| Source location + diagnostics | Yes (getLine/getCharPositionInLine, LogError) | reuse |

Note on threads: there is no `thread<T>` language construct. Threads are the `Thread`
struct (core/thread.cb) over os.thread_create, plus the `program` run-thread spawn
(MainListener.h:19681,20164). Any doc saying `thread<T>` means those.

## Milestones (RE-ORDERED per Fable review: A first, then B, then C)

1. A, runtime contention profiler: `__lock_profile_hook` + core/diagnostic/lock_profile.cb
   + siteId table + acquire/release instrumentation. NEAR-ZERO RISK (clone of the proven
   __fuzz_hook / thread_fuzz.cb pattern, no soundness questions), delivers immediately to
   the live HPC/B-tree work. Independent of B/C. (Was milestone 2.)
2. B, static lock-order checker: graph accumulated DURING the codegen walk (NOT a
   ForwardRefScanner clone) + SCC + `--check-lock-order`, mode-aware edges, scoped-lock +
   RequiredLocks only (raw path deferred to C). Architecturally the standout, but its
   value to the DRIVING use case is capped until C exists: the B-tree's canonical pattern
   lands squarely in B's same-class self-edge blind spot, so B's flagship demo produces
   caveated warnings, not proof. (Was milestone 1.)
3. C, hand-over-hand validator: hardest and most valuable to the concurrent B-tree.
   Blocked on the loop-merge (invariant-check, not fixpoint - shared with move-safety
   soundness) and on same-class self-cycle identity. Build on lock-hand-over-hand.md.
   Feed C's raw-path held set back into B's graph.

Note: A and B touch disjoint areas (core/diagnostic + one codegen hook vs. graph in
LLVMBackend), so they can run in parallel.

## Open questions / risks

- Loop back-edge handling: held-lock typestate across while/for/do/switch has no
  snapshot/merge today (ParseControlledBody is a single linear walk). NOT a fixpoint
  (must-analysis): loop-head snapshot + single walk + back-edge SUPERSET-OR-EQUAL-head
  invariant check. Build as ONE generic per-variable-flow-state mechanism but ENFORCE
  for locks only in the first change; adopt move-state later (turning loop-merge on for
  IsMoved is a behavioral tightening - file the latent move-in-loop hole in
  internal/issue/ separately). Detail in lock-hand-over-hand.md.
- Per-object vs per-class identity for SAME-CLASS self-cycles: the canonical B-tree
  hand-over-hand path acquires BTreeNode.mtx while holding BTreeNode.mtx - a self-edge
  in B's graph. Static identity is class/text only, so B cannot distinguish "different
  node, safe coupling" from "same node, real deadlock". Options: suppress self-class
  edges inside a validated hand-over-hand region (C), require a source annotation, or
  rely only on the consistent-order argument. Undecided.
- Matching raw acquire/release pairs across control flow: `m.acquire()`/`m.release()`
  are ordinary method calls not tracked in currentLockSet today. B and C must recognize
  calls whose callee is the Acquire/Release method of a capability type and pair them
  across branches/returns - double-release and lost-release hazards, especially against
  the scoped lockCleanup teardown (double-release with an early unscoped release).
- Profiler siteId for the raw path: hooks inside acquire/release see the CALLEE site;
  attributing to the CALLER site needs the compiler to thread a site id through, or
  accept callee-site granularity for raw locks.
- Sub-region hold-time attribution (A) needs extra instrumentation points inside the
  critical section (statement-level hooks) - cost vs value TBD.
- rwlock shared/optimistic modes: shared holders overlap, so wait/hold semantics and
  order-graph edges for read locks need mode-aware rules (LockMode already carries
  Shared/Exclusive/Optimistic).

## Related

- internal/plan/lock-hand-over-hand.md (typestate-on-pointer design, the loop-merge gap
  this plan's C builds on)
- internal/plan/lock-capability-interface.md (capability model; why raw acquire/release
  stays legal)
- internal/plan/lock-guard-mandatory.md (release-on-early-return, mode fix)
- internal/plan/optimistic-lock-coupling.md (IOptimisticLockable, lambda read path)
- core/diagnostic/thread_fuzz.cb, core/diagnostic/heap_audit.cb (runtime hook-shim
  precedent for tool A)
