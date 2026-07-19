# Move-state dataflow: sound use-after-move across all control flow (Fork A / A1)

Status: DESIGN, decided. Supersedes the conservative "Fork B" back-edge patch. Replaces
the current inline, single-linear-pass move check with a proper MaybeInitialized dataflow
fixpoint, so use-after-move is sound across loop back-edges, switch, break/continue, and
nested control flow - not just if/else.

Decisions (maintainer):
- A1 full-function dataflow pass (NOT the loop-scoped A2 replay).
- Index/deref moves stay PERMISSIVE: `move arr[i]` / `move *p` get no move-path (untracked,
  neither checked nor flagged), unlike Rust which rejects them. core/ relies on this
  (list/queue/stack grow loops, dictionary, hpc/btree slot moves).

## How Rust does it (the model we follow)

- Lowers to MIR, a CFG of basic blocks. All move analysis runs as iterative dataflow on
  that CFG - this is the piece CFlat lacks.
- Move paths: every local gets one; struct fields get sub-paths (moving `a.0` leaves `a.1`
  live - same as CFlat MovedFields). Array elements `arr[i]` and `*ref` get NO move-path
  (cannot move out of an index in Rust).
- `MaybeInitializedPlaces`: forward dataflow over a join-semilattice. gen = init/assign,
  kill = move. join = UNION at merges. Iterates over the CFG (incl. back-edges) to a
  fixpoint; finite lattice height guarantees termination.
- A use of a maybe-uninitialized (maybe-moved) place is E0382. Loops are handled for free:
  the back-edge carries moved-state to the loop header, so a use at the top is caught -
  Rust says "value moved here, in previous iteration of loop".
- Reassignment revives (gen re-inits); conditional move is maybe-init at the merge and
  errors on later use.

Refs: rustc-dev-guide borrow-check/moves-and-initialization(/move-paths) and mir/dataflow.

## CFlat mapping

| Rust | CFlat today |
|---|---|
| MIR CFG of basic blocks | none - single-pass hand-written AST->LLVM walker (MainListener.h) |
| move-path (local + field subpaths; none for arr[i]/*r) | var name + NamedVariable.MovedFields; index/deref already untracked |
| gen=init / kill=move | MarkVariableUnmoved (14717) / MarkVariableMoved (14689), field variants 14733/14746 |
| MaybeInit forward fixpoint, join=union, over back-edges | ONLY if/else merges today (MainListener.h:5182-5202); loops do NO merge |
| use of maybe-moved -> E0382 | inline MovedUseSubject (LLVMBackend.h:14763) at loads |

The one missing piece is the iterative dataflow over a CFG. That engine is also the
"possibly-X at a join point" merge the lock hand-over-hand typestate needs - build ONCE,
share (move-state = MAY/union; held-locks = MUST/intersection - same solver, opposite meet).
See internal/plan/lock-hand-over-hand.md, internal/plan/lock-checker.md.

## CFlat-native realization (the crux) - RUN THE FIXPOINT ON THE LLVM CFG

Do NOT re-derive moves in a pre-codegen pass: whether a call argument is a move depends on
overload resolution, which only happens in the main codegen walk. Re-implementing that is
the "two copies must agree" divergence hazard.

Do NOT hand-build a parallel analysis CFG either. The codegen walker ALREADY emits a REAL,
complete LLVM CFG - one llvm::BasicBlock per if/loop/switch/short-circuit region, with every
branch edge (including loop back-edges, break->resume, continue->latch, switch fallthrough,
&&/|| diamonds) already wired by existing codegen. LLVM hands us that graph for free via
`BasicBlock::successors()` / `predecessors()`. And the walker already knows the RESOLVED
move/gen/use facts (it calls MarkVariableMoved / MarkVariableUnmoved / MovedUseSubject with
overloads resolved). So the pass is just: tap the events, tag each with its block, solve on
LLVM's CFG.

1. TAP the move-event sites (see the wiring map in this dir / the recon). At each KILL
   (MarkVariableMoved / MarkVariableFieldMoved / MarkVariableMovedIntoInterface), GEN
   (MarkVariableUnmoved / MarkVariableFieldUnmoved, plus each variable declaration = initial
   GEN), and USE (MovedUseSubject call sites), append an event record
   {block = builder->GetInsertBlock(), kind, movePath, sourceLoc} to a per-llvm::Function
   event log, IN EMISSION ORDER (walk order within a block is already linear/correct).
   movePath = var name, or name+field; index/deref/any non-name-or-static-field lvalue =
   NO path (untracked) per the permissive decision. A whole-var kill kills the var path and
   all its field subpaths; a field kill kills only that subpath (mirror MovedUseSubject).
2. After the function body is fully emitted (all branches placed, terminators closed - i.e.
   at the function-exit point, ~MainListener.h:5932-5973, before the frame pops), run a
   forward worklist fixpoint over the llvm::Function's basic blocks:
     - lattice per block: set of maybe-moved move-paths (MAY / union).
     - entry block IN-state: params live (empty maybe-moved set).
     - block IN = UNION over predecessors' OUT (this is the join; back-edges included).
     - block OUT = transfer(IN, that block's ordered events): apply gens (erase path) and
       kills (insert path) in order.
     - iterate the worklist until no OUT changes. Finite path set => terminates.
3. DIAGNOSE: replay each block's events against its solved IN-state in order; at a USE whose
   move-path is in the maybe-moved set, LogError. When the reaching kill arrived via a
   back-edge (the block is inside a loop and the kill dominates the latch), phrase it as a
   loop-iteration move ("value moved in a previous loop iteration"); else the ordinary text.

Why this is robust: no control-flow re-derivation (LLVM owns the CFG), no second move
detector (events come from the resolved walk), and untapped runtime-lowering diamonds simply
carry no events (identity transfer). Per-llvm::Function keying handles nested lambdas (each
has its own Function). Name-keying matches the existing SaveMovedState behavior (a known,
pre-existing shadowing limitation - do not "fix" it in Stage 1).

## Rollout (staged - each stage independently verifiable with ./test.sh)

Stage 0 (DONE): ~~internal/issue/move-state-loop-merge.md~~ **FIXED 2026-07-18, issue file
deleted.** It recorded the gap (loop back-edge silence read as sound when it was not); this file
is the design that closed it.

Stage 1 (DONE): engine, verify-only, NO behavior change. cflat/MoveDataflow.h holds the
per-llvm::Function MAY/union fixpoint over the emitted LLVM CFG; LLVMBackend records move
events (kills inside MarkVariable*Moved, gen-revives inside MarkVariable*Unmoved, gen-binds
at all 8 binding sites, uses at the 5 MovedUseSubject sites), keyed by llvm::Function*.
`&v` is a gen-revive (out-param reinit); subscript results are IsElementAccess (untracked,
the permissive index decision). Verified: ./test.sh Release green + 0 divergences on the
surveyed idioms + the loop-carried bug detected under CFLAT_MOVE_DF_VERIFY.

Stage 2 (DONE): flip to real errors. RunMoveDataflow (LLVMBackend.h) reports the earliest
divergence via LogError("use of moved variable '{}' (moved on an earlier loop iteration)");
called INSIDE the top-level try (LLVMBackend.cpp:~785) so a file-scope bare expect_error
matches it. The inline checker is untouched and still owns straight-line/if-else. Double-
reporting is avoided in the analyzer itself: AnalyzeFunction computes BOTH the full fixpoint
AND an acyclic (forward-RPO-only, no back-edge) pass, and reports a use ONLY when maybe-moved
in the fixpoint but NOT in the acyclic view = genuinely loop-carried. Analysis is restricted
to entry-reachable blocks (stale post-abort blocks from a caught scoped-block expect_error
must not leak). Escape hatch: CFLAT_MOVE_DF_OFF disables the pass. Regression:
Test/errors/err_use_after_move_loop.cb (file-scope bare form). Verified: ./test.sh Release
391/0/8, no regressions.

>> The loop-merge soundness gap (~~internal/issue/move-state-loop-merge.md~~ **FIXED
>> 2026-07-18, issue file deleted.**) is CLOSED by Stages 1+2.

Stage 3 (BLOCKED / discretionary - NOT done): share the engine. parameterize the meet (union
for move MAY-state; intersection for lock MUST-state) and reuse it for the lock hand-over-hand
held-lock typestate. BLOCKED: the lock typestate feature is unbuilt (lock-hand-over-hand.md is
still design) - there is nothing to share the engine WITH yet; do Stage 3 as part of building
that feature. The sub-idea "retire if/else's bespoke Save/Restore/Merge (14795-14848) onto the
one solver" is DISCRETIONARY and risky: it would force the dataflow pass to own ALL use-after-
move reporting (straight-line + if/else), reintroducing the per-function reporting-timing
problem vs scoped-block expect_error. No functional benefit today - defer unless the shared
engine lands.

## Must-not-regress idioms (from the corpus survey - all must stay clean)

- Linked-list free loops (reassign revives): `... = p->next; delete p; p = next;` in
  Test/vectorize/neg_while_noncountable.cb, core/arena.cb, arena_channel.cb, json.cb, xml.cb.
  Sound under the fixpoint: `p = next` is a gen (re-init), so p is live at the back-edge.
- foreach deleting the element (`for (IElement c in ...) delete c;`): the element is a fresh
  binding each iteration - a gen at loop top; never maybe-moved at the use.
- Fresh owning local moved/deleted per iteration (Test/test_program.cb, test_move.cb): the
  in-loop declaration is a gen dominating every use.
- Indexed container-slot moves (list/queue/stack grow, dictionary, hpc/btree
  `newData[i] = move _data[i]`): untracked (no move-path) - never flagged.
- `while (p != nullptr) { consume(move p); }`: relies on the runtime-null crutch. The static
  fixpoint WOULD flag this (p maybe-moved at the guard's read on iteration 2) - it is a REAL
  static-vs-runtime gap. Confirm against the suite; if such a self-terminating idiom exists in
  a POSITIVE test, that is the one place we must decide: accept the conservative error and
  require a reassign, or special-case a move whose kill is immediately followed by loop exit.
  The survey found none in positive tests (only the reassigning linked-list form), so Stage 2
  is expected green; keep this risk on the radar.

## Open questions

- Program-point granularity of a "use": per-statement is enough for the diagnostics we emit;
  finer (per-subexpression) only matters for move-then-use within one statement, already an
  inline error today.
- Diagnostic quality for the back-edge case (which move site to point at - the one on the
  prior iteration). Record the kill's source location on the analysis node.
- Interaction with the existing MovedIntoInterface / MovedFields / IsBonded flags - the pass
  must carry the same sub-states, not just a single moved bit, or targeted messages regress.
- Whether Stage 2 can keep the inline checker for straight-line (single-block) uses and let
  the pass own only multi-block/back-edge uses, to shrink the blast radius. Cleaner long-term
  is one owner (Stage 2 as written); revisit if Stage-2 verification surfaces churn.

## Related

- ~~internal/issue/move-state-loop-merge.md~~ **FIXED 2026-07-18, issue file deleted.** (the
  gap this closed: use-after-move across a loop back-edge was unsound silence, not a proof)
- internal/plan/lock-hand-over-hand.md, internal/plan/lock-checker.md (share the merge engine)
