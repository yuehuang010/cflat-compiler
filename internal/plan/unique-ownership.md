# Unique ownership: fields, locals, params, containers, and interface values

Consolidated 2026-07-19 from two plans, and again 2026-07-20 from a third. All now deleted:

- `field-ownership-unique.md` (2026-07-16) - Part I below: the `unique` FIELD qualifier.
  Stages 1-5 DONE, including the code-review remediation and the interface field contract.
- `interface-value-ownership.md` (2026-07-18) - Part II below: interface value ownership and
  `unique` in GENERIC-ARGUMENT position (`list<unique X*>`, `list<unique IShape>`), plus
  unique locals and synthesized move params. Stages 1-6 DONE; Stage 7 (D12) was attempted,
  rolled back 2026-07-19, and superseded. Its dead-end record was deleted 2026-07-20 - the
  final design has landed and D12 must not be revived.
- `unique-field-migration-survey.md` (2026-07-20) - the census that CANCELLED the core field
  migration. Its durable findings are inlined into NEXT item 1 and "Rules that still bind";
  nothing else in it was load-bearing. Deleted 2026-07-20 as part of this consolidation.

**This is the single plan for the `unique` workstream. Do not start a second one.**

Parts I and II were COLLAPSED TO THEIR SETTLED DECISIONS on 2026-07-20, and the dead-end and
cancelled-proposal records deleted, now that the final design has landed. The stage-by-stage
narrative, design rationale, prior-art surveys and code-review logs are all in git history.
Corrections to WRONG diagnoses are kept - they stop a fixed bug being re-derived.

## Status and remaining work (the live section)

Last updated 2026-07-20. Everything above the "Completed" ledger is work that is still open;
the ledger is a one-line-per-item record of what landed. The detailed blow-by-blow of each
completed migration was removed on 2026-07-20 to keep this section readable - it is preserved
in git history, and the durable LESSONS from it are hoisted into "Rules that still bind" below.

### RESUMING FROM A FRESH CONTEXT - read this first

Written 2026-07-20 at the end of a long working session, so the next context can pick up
without re-deriving anything. Delete this subsection once its contents are stale.

**Where the tree stands.** Last commit is `4cce536`. Everything through the six-type RAII
migration is now COMMITTED (`54d6803` = the six container migrations; `4cce536` = the RAII work
and the rest of the 2026-07-20 batch). The ledger rows below carry the real commit; rows still
marked "uncommitted" are genuinely uncommitted working-tree edits.

Uncommitted in the working tree, verified green, no agents in flight:

- `dictionary<K, unique V*>.add()` duplicate-key leak FIXED (`cflat/core/dictionary.cb`), with a
  regression leg in `Test/test_collection_leaks.cb`. Its issue file is deleted in the same edit.
- Synthesized destructor's SCALAR arm widened to `IsUnique || IsUniqueTypeArg`
  (`cflat/LLVMBackend.h` ~3079), plus two regression legs. See NEXT item 3.

**A commit was made in error on 2026-07-20 and undone with `git reset --soft`.** An implementation
agent committed and fast-forwarded onto `master` despite the standing "do not commit" rule. The
change survives as staged working-tree edits. If a future session finds staged-but-uncommitted
`unique` work with no explanation, this is why - it is not an abandoned experiment.

**Verified baselines to measure against:** `bash test.sh Release` = 448 passed, 0 failed,
8 skipped. `bash test_example.sh` = 35 passed, 0 failed. `bash test_lsp.sh` = 152 passed, 0 failed
(449 faded hints). macOS host - do NOT add "needs Windows verification" caveats; the maintainer
owns that and is aware.

**Standing instructions from the maintainer.** Do NOT commit (stash is allowed); keep `master`
linear, no merge commits. Use the repo-root `scratch/` for ALL temp files. Never use the `haiku`
agent tier. Do not create new `Test/*.cb` files (`Test/errors/err_*.cb` IS sanctioned). Do not
revert changes to check a baseline - ask first. Do not weaken test assertions to make something
pass. The maintainer's standing steer this session was "continue until blocked" and "do not let
commit block you" - so keep batches logically separable for slice-committing rather than idling
on commit boundaries.

**THE THREE PENDING DECISIONS WERE ANSWERED 2026-07-20** - see NEXT items 3 and 4 for the
full rulings. In short: (1) the synthesized destructor will release `unique` fixed-array fields
element by element, which is BREAKING for `btree` and must migrate it in the same change
- LANDED 2026-07-20, see the ledger;
(2) core's manual-lifecycle types go RAII, with copy suppression handled by AUDIT-AND-DISCIPLINE
for now - extending `unique` to non-`delete` releases is the preferred long-term answer but was
explicitly NOT chosen yet - LANDED 2026-07-20 for six types, see the ledger;
(3) `~Thread()` on a still-running thread is an ERROR (compile-time
ideal, runtime acceptable), never a silent join or detach - NOT IMPLEMENTED: the audit found a
real copy path AND a deliberate detach pattern in `channel.operator>>` that the ruling's own
semantics would abort. See `internal/issue/p3/thread-cannot-go-raii.md`.

**Process lessons from this session - these cost real time, do not relearn them.**

- **VERIFY EVERY AGENT CLAIM INDEPENDENTLY.** Across ~8 agents, THREE filed root causes turned
  out to be wrong and were only caught by re-running the repro by hand. The corrected diagnoses
  are recorded in "Resolved 2026-07-20" below and are worth more than the fixes themselves. An
  agent reporting green is not evidence; re-run the suite and the specific repro yourself.
- **Agents that push back are usually right.** Four agents this session contradicted part of
  their brief (the field-migration survey found ZERO of a supposed 34 targets; another proved a
  filed root cause wrong; another refused a "fix" that would have broken shipping code). Every
  one of them was correct. Write briefs that invite this explicitly.
- **Do NOT run two agents in parallel when they share an acceptance gate.** Disjoint SOURCE
  files are not disjoint when both edit the same test file or both gate on the full suite. This
  polluted one agent's gate and produced a phantom failure it correctly refused to own.
- **Split multi-bug issue files.** Three findings once lived in one file whose own instructions
  said "delete when fixed" - fixing the titular bug would have silently discarded the other two.
- **A counting oracle cannot distinguish "both correct" from "both inverted."** This hid a fully
  inverted move-overload pair for hours. Assert identity or an observable effect, never counts
  alone.

**What is genuinely finished.** All six containers (`list`, `hashset`, `dictionary`, `btree`,
`array`, `queue`/`stack`) are on ONE rule. The field migration is CANCELLED with evidence, not
deferred. `alias`-as-type-argument is removed. What remains in NEXT is either blocked on a
decision above or is the low-priority optimisation in item 5.

### THE RULE (settled, and now uniform across core)

**BARE BORROWS, `unique` OWNS - pointers and interface fat pointers alike.**

| Spelling | Meaning |
|---|---|
| `C<T>` (value) | element is COPIED on insert; `move` at the call site transfers instead |
| `C<T*>` | BORROWED - the container never frees it, the caller owns it |
| `C<IShape>` | BORROWED - an interface value is a fat pointer and follows the pointer rule |
| `C<unique T*>` | OWNED - freed on overwrite, on removal, and at teardown |
| `C<unique IShape>` | OWNED - same, through the vtable dtor slot |

Keys in keyed containers stay BORROWED (`alias K`): a key is read for comparison on every
lookup, and an owning key would be destructively moved out by that read.

The goal this serves, in the maintainer's words: *"The more important goal is to align usage of
ptr and fatptr."* Ownership reads off the type at the local declaration site, with nothing at
the call sites - C#-like local readability, with the complexity paid once in the library.

### DIRECTION CHANGE 2026-08-31: `unique` becomes a LIBRARY TYPE `struct unique<T>`

Maintainer ruling: the builtin `unique` modifier is causing too many stabilization headaches.
Convert it to a C++-style library type `struct unique<T>`, keeping the `unique` keyword as sugar
for it. Constraint from the maintainer: **the helpful diagnostics of the builtin must survive
the conversion** - there is a balance between library genericity and compiler-blessed errors.

**Prototype results (2026-08-31, worktree unique-type-prototype-4ba544, probes in `scratch/`):**

A hand-written `uptr<T>` (ctor `uptr(move T* p)`, dtor, `operator->`, `get/release/reset/valid`,
`operator!`) works TODAY with zero compiler changes:

- Scope-exit destruction, factory return, `move` param consume, `release()`/`reset()`,
  move-between-locals: all correct dtor counts, no double frees (`scratch/uniq_proto.cb`).
- `struct unique<T>` is LEGAL as a name despite the soft keyword, and COEXISTS with the builtin
  `unique T*` qualifier in the same file (`scratch/coexist_probe.cb`) - incremental migration
  is viable, no big-bang rename needed.
- `list<uptr<Resource>>` works: `xs[1]->id` chains `operator[]` + `operator->`, elements
  destroyed exactly once at teardown (`scratch/container_probe.cb`).
- `b = a` on an owning value type is an implicit move that NULLS the source, and the move
  dataflow tracks it for direct field reads (`a._p` after -> "use of moved variable 'a'").

**The diagnostic gap, measured (this is the whole balance problem):**

| Misuse | builtin `unique T*` | library `uptr<T>` |
|---|---|---|
| read after explicit move | compile error "use of moved variable 'p'" | compiles, SEGFAULT at runtime |
| init from borrowed value | compile error "cannot initialize unique 'b' from a borrowed value..." | n/a (ctor takes `move T*`) |
| direct field read after move | (same guard) | compile error - already works |
| method/operator call on moved receiver | compile error | compiles, runs on zeroed shell |

The gap is NARROW: move dataflow checks direct field reads on a moved owning-struct local but
NOT a method/operator call where the moved local is the RECEIVER (`a->id` via `operator->`,
`a.valid()`). Fixing that one dataflow hole gives library `unique<T>` the builtin's
use-after-move story, and benefits EVERY owning value type (ComPtr, list, ...), not just
unique<T>. Related existing issue: `p2/deref-of-moved-pointer-guard-inside-callee.md`.

**Guiding balance rule (from the maintainer 2026-08-31): keep the builtin's helpful errors.**
Operationalized as: compiler blessing of `unique<T>` is limited to DIAGNOSTIC WORDING and
source locations. Semantics (destruction, move-by-default, consume rules) must fall out of
the struct definition plus the GENERAL owning-value-type rules, so every rule that makes
`unique<T>` safe also makes `ComPtr<T>` and user RAII types safe. If a stage needs a
semantic special case keyed on the name `unique`, that stage is designed wrong - stop and
re-derive it as a general rule.

### MIGRATION PLAN - `unique` qualifier -> library `struct unique<T>` (2026-08-31)

Ordering principle: each stage lands green on the full suite and is independently useful;
the builtin keeps working unchanged until Stage 5. No stage breaks user code before Stage 4.

**WHERE THINGS STAND (2026-09-01, end of overnight session).** Stages 1-3 are DONE and live
as THREE single-parent commits on branch `claude/unique-type-prototype-4ba544` (worktree of
the same name): `7139375` receiver-gap fix, `6cc6537` core unique<T> (cherry-picked from
the retired stage2 branch), `3530fdd` diagnostic blessing. The branch fast-forwards onto
master; maintainer wants to INSPECT before it goes to master - do not merge without them.
The stage2 worktree/branch (`unique-stage2` / `claude/unique-stage2-core-type`) is
superseded by the cherry-pick and can be deleted at inspection time. Maintainer rulings
this session: `unique<int>` stays ALLOWED (possible path to atomics - do not poison
primitives); helpful builtin errors must survive the conversion (the balance rule above).
Suite baseline on the branch: 756/0/8, examples 45/0, LSP green.
TIMEBOX CLOSE-OUT (2026-09-01 ~05:30, end of the maintainer's 5h box). On the branch, all
suites green (762/0/8, examples 45/0, LSP green): Stage 4a (interface arm, d2e93e6), Stage
4b LOCALS desugar incl. indirect-call adaptation (d5b247b + 495c0f4), the dbnl helper-gap
issue fix (a128abd), Stage 3 partial recorded on the move-sink issue. NOT done: 4b params /
fields / generic-args / interface-locals slices (a ready-to-run brief for params+fields is
in scratch/codex_stage4b2_brief.md), Stage 5 removal slices (ordered map below). Next
session: run 4b2 brief, then generic-args + interface slice, then Stage 5 order 1-7.
RULING 2026-09-01 (maintainer): Stage 4 starts now. The interface sub-question is decided:
`unique<IShape>` gets an `if const (is_interface(T))` arm inside `core/unique.cb` holding
the fat pointer and releasing through the vtable dtor slot - full parity, one type. The
partial-desugar and reject-interfaces alternatives were declined.

TIMEBOX CLOSE-OUT (2026-09-01 ~18:30, end of the maintainer's 12h box). STAGE 4 IS DONE:
every user-facing `unique` spelling desugars to library `unique<X>`. Commits this box, all
single-parent on `claude/unique-type-prototype-4ba544`, all verified green (test.sh Release
764/0/8, test_example.sh 45/0, test_lsp.sh 180/0): `d119b11` 4b params+fields, `24335ed`
4b generic-args, `1d8239c` 4b interface spelling, `1d32664` Stage 5 slice 2 (is_unique
re-point to IsCoreUniqueType), `e7b81ca` Stage 4 scanner residues (interface type-args,
using-alias closure bases, fixed-array unique fields), `7198859` Stage 5 slices 4-5 (dtor
arms, TypeOwnsUniquePointer re-key to HasNonTrivialDestructor, six void* slots dropped -
see scratch/unique_void_resolution.md), `f8fde6d` Stage 5 slice 3 (kUniqueQualifierPrefix +
`unique_` mangling token retired; reject paths now diagnose in the main pass only).
Ruling made this box: reading a blessed unique<X> ELEMENT into a local is a BORROW-COPY
ALIAS (non-owning, no dtor); explicit `move` is the only consuming spelling
(MainListener_Expressions.cpp ~3078).
STAGE 5 REMAINDER = slices 6/7/1, MEASURED AND BLOCKED, not attempted-and-failed: the
`preserveBuiltinUnique` paths are still LIVE (cold-trace counts: 97 interface, 7 raw-array,
5 aligned-field consumer hits; main-qualifier producer 742). A trial full removal broke
unique__IShape reset overload resolution, borrowed-value rejection on an all-null
conditional, and crashed generic instantiation (unique__Sq/unique__Ci) - rolled back.
So: IsUnique (cache key "uq") STAYS, the ParseDeclarationSpecifiers text-match plumbing
STAYS (the desugar rewrite itself is permanent regardless), IsUniqueFieldAlias stays as the
desugared-path carrier (1184 carrier hits vs 8 builtin hits, all aligned fields).
Definitive builtin remnants: (1) alignas'd unique fields (permanent until unique<T,ALIGN> -
plain delete on aligned block = heap corruption), (2) four reject-path scanner spellings
(unique int / unique array-view / two closure forms in err_lambda_array_view.cb), (3) the
IsUnique flag + "uq" serializer key serving (1)+(2) and the still-live preservation paths,
(4) IsUniqueFieldAlias carrier. NEXT SESSION: root-cause WHY the preservation paths still
fire 97/7/742 times on a fully-desugared tree (suspect: generic instantiation re-parses
carry the builtin spelling internally) - that is the key that unlocks slices 6/7; the
batch-3 brief (scratch/codex_stage5_batch3_brief.md) and run log
(scratch/codex_s5b3_run.log) carry the measurement detail. Never merge to master -
maintainer inspects first.

STAGE 5 BATCH 4 (2026-09-01, session after the 12h box). ROOT CAUSE of the "still-live
preservation paths": the generic re-parse suspect above was WRONG. Nothing carried the
builtin spelling internally. The 200 `preserveBuiltinUnique` rollbacks came from six
textual heuristics in ParseDeclaration, each standing in for a desugar adaptation the
library type or compiler lacked (per-tag cold-trace taxonomy with disable probes:
scratch/stage5_preserve_taxonomy.md). Commit `704c510`: seven heuristics measured dead
deleted; the broad move-name fallback (83 hits) hid a compiler SEGFAULT (core-unique
wrapper values reaching the raw cast path unboxed) - fixed, not masked; dictionary set
now shares the p.get() borrow adaptation (43); interface pointer/conditional assignment
boxes before reset (63); `?:`/`??` in move-return position release the selected
unique<T> arm and leave the untaken arm for one scope-exit destruction (4); a wrapper
owning a counted `new T[n]` result frees with delete[] (probe: 3 dtors, no double free).
Verified independently: test.sh Release 764/0/8, examples 45/0, LSP 180/0. Final trace:
every tag 0 except the two RULED REMNANTS: (1) alignas allocation moved into a local
(4 sites, test_core.cb:2943/3089/3108/3146) - permanent until unique<T, ALIGN>;
(2) owning heap array through a unique pointer (`unique T* v = new T[n]; v[i]`, 6 sites,
test_core.cb runRawCount*) - NEEDS MAINTAINER RULING on the surface (unique<T[]>? a
separate owning-array type? keep builtin?) before any code. Retarget:
err_discard_move_use_after "use of moved variable 'p'" -> "dereference of moved variable
'p' (it is null after the move)". Slices 6/7 (IsUnique flag + "uq" key, qualifier
plumbing) are now unblocked EXCEPT for what the two remnants, the alignas'd unique
fields, and the four reject-path scanner spellings still need - re-measure IsUnique
consumers before touching them. Never merge to master - maintainer inspects first.

STAGE 5 BATCH 5 (2026-09-02, slices 6/7). Commit `aae5ca5`: per-site cold trace over ~90
IsUnique/IsUniqueTypeArg reads (scratch/codex_stage5_batch5_report.md has the table).
70 zero-hit consumer arms deleted; 40 arms re-keyed on IsCoreUniqueType / IsMove /
pointer-interface shape; IsUniqueTypeArg, ElementOwningUnique, IsBorrowOfUniqueElement
deleted with serializer keys unt/eou/bue. IsUnique + "uq" STAY, now serving only: R1
alignas'd unique fields, R2 alignas allocation moved into a local, R3 owning heap array
through a unique local (ruling still open), R4 four reject-path spellings, R5 union-member
recovery, R6 pointer-form brace/assignment diagnostics, plus the interface unique-field
ABI contract. Retargets: borrowed-value rejections now spell the desugared type and say
"reset" for assignment (err_move, err_move_borrowed_ptr_into_unique_field,
err_unique_borrow_into_unique, err_unique_iface_borrow_no_move, err_unique_iface_stack_box).
Verified independently: test.sh Release 764/0/8, examples 45/0, LSP 180/0. Review debt
before master: the predicate `IsMove || (!Pointer && IsCoreUniqueType(name))` was copied
nine times - folded into IsMoveOrCoreUniqueValue in `ba752a2`. Process lesson (4h25m run, 1147 turns, 114 builds):
never brief "delete + retarget" in one run; zero-hit deletions first, then one run per
retarget cluster, with a 3-attempt stop rule and the lldb path mandatory. STAGE 5 IS
NOW COMPLETE except the R3 ruling. Never merge to master - maintainer inspects first.

STAGE 5 R3 RULING (2026-09-02). Maintainer: "Block unique<T[]> for now" - narrow block, keep
the raw-count tests. unique never owns a heap array via the library type: `unique<T[]>` is
rejected at all three instantiation sites (ParseDeclarationSpecifiers, ResolveTypeArgEntry,
QueueGenericInstantiation) with the existing array-view wording; a builtin-preserved
`unique T* r = new T[n]` returned as `unique<T>` is a LogError (was an LLVM verifier
failure); `new T[n]` stored into a `unique T*` FIELD is a LogError (was delete of 1 of n).
Builtin `unique T* v = new T[n]` with per-local count metadata is a PERMANENT remnant, so
the arrayRebind heuristic and IsUnique/"uq" stay; Test/test_core.cb runRawCount* (38
assertions) unchanged. Legs in Test/errors/err_unique_array_view.cb; both p2 issues deleted.
Generic `T[]` args in general are NOT blocked (list<int[]> fails only because the body
forms `int[]*`; follow-up internal/issue/p3/generic-array-view-arg-diagnostic-points-into-core.md).
Verified: test.sh Release 764/0/8, examples 45/0, LSP green, warm-cache legs fire.

STAGE 5 OPUS REVIEW ROUND 1 (2026-09-02, after the R3 ruling). Read-only Opus pass over
`git diff master...HEAD` (brief scratch/opus_review_brief.md): 8 CONFIRMED, 4 PLAUSIBLE, all
applied in three Codex runs split by outcome class. `3bf443e` heuristics: the R2/R3 remnant
decision is now parse-tree based (new-array expression on the declarator's own initializer or
a later `=` to that exact name; move source itself carries AllocAlignValue); the `find("align")`
/ `find("new")+find("[")` / `body.find(name+"=")` probes are gone from both passes. `d3ec811`
memory safety: unique<IFace>.copy() double free (fat `_v` field escaped both copy gates),
reset() self-safe, dead "unique " prefix strip removed. `5b167a4` diagnostics: borrowed source
named again via one BorrowedSourceName helper (agnostic wording when unknown - an IR-walking
name recovery Codex tried was rejected), overload listings and the wrapper copy() rejection
show unique<T> spellings. Verified per commit: test.sh Release 764/0/8, examples 45/0, LSP
green, warm-cache legs. Round 2 review scoped to 7ee554c..HEAD follows.

STAGE 5 OPUS REVIEW ROUND 2 (2026-09-02, scope 7ee554c..HEAD). 8 CONFIRMED, 1 PLAUSIBLE.
`0b48e3e` remnant decision: scanner vetoes desugar on ANY alignment specifier (its folder only
read literals); later-assignment walk skips blocks/for-inits that re-declare the name; aligned
move source resolved from the parse tree in scope order, not the live symbol table; return
diagnostic takes the name from the move expression. `2aa0c07` provenance: one
HasRawNewArrayProvenance gate for return / field store / argument boundary, args carry array
provenance; the `move T*`-parameter-into-unique-field leg stays OPEN (needs a per-parameter
"stored into unique field" fact) - internal/issue/p2/unique-field-heap-array-through-move-param.md.
Clean per review: reset() interface arm makes no second owner; fat-vs-fat interface compare is
identity; multi-declarator reset; new-array discrimination structural; UFCS copy unaffected.
`7109771` diagnostics: StructSynthCopyUnsafe dead arm and the discarded _v/_p path dropped; DisplayNameOfMangledType takes the core-unique arm only for arity-1 outer templates (Pair__unique__Node__i32 now returns raw, not writable); range-for over unique<T> elements and alias/array unique<T> fields no longer print unique__X or ._p. Round 3 review scoped to 5b167a4..HEAD follows.

STAGE 5 OPUS REVIEW ROUND 3 (2026-09-02, scope 5b167a4..HEAD). 5 CONFIRMED, 2 PLAUSIBLE, one
Codex run, `6048c16`: the round-2 presence-based alignas veto in the scanner had diverged from
the main pass for single-arg `alignas(N)` on a unique parameter/return (undefined symbol at
link) - both passes now fold the allocation argument with FoldCompileTimeInt; the heap-array
argument rejection fires only on an actual transfer (borrowing an array local into a unique<T>
parameter compiles again); copy() diagnostic back under LogErrorMessage as two templates and
shows Box<unique<N>>; DisplayNameOfMangledType returns raw/non-writable when the outer template
is unregistered. Plausible, unfixed: hasAlignedMoveSource gives up across lambda boundaries and
on namespace mismatch (no reproducer reaching a bad free). Per the review-loop rule a Fable
advisor pass replaces a 4th Opus round; its verdict is recorded below.

FABLE ADVISOR VERDICT (2026-09-02, after round 3): NOT converged. (1) Round 3's `transfers`
gate re-opened a hole: a by-value unique<T> parameter CONSUMES, so `peek(a)` with an array
local (no `move`) compiled and freed 1 of 3 (scratch/rev4/g1.cb); the Opus finding's "borrow"
premise was wrong and the test leg it added exercised a raw `Resource*`, not a unique<T>
parameter. Corrected in the follow-up commit (unconditional rejection restored, implicit-form
leg in err_unique_array_view.cb). (2) STRUCTURAL CAUSE, maintainer decision needed: the R2/R3
remnant decision in MainListener::ParseDeclaration (~3362-3644, ~280 lines of name/token-index
parse-tree lookahead, plus a third copy of the rule in ForwardRefScanner ~365-395) is a second
syntactic ownership analysis that runs ahead of the real one and does not share its provenance
flags; every round found a shape it misses. Recommendation: REPLACE - (a) `new T[n]` into a
unique LOCAL becomes a hard error like the other legs (drops hasRawNewArrayValue and
hasLaterBuiltinUniqueAssignment), (b) builtin stays only when the DECLARED type carries
`alignas(_, N>0)`, which both passes now fold identically (drops hasAlignedMoveSource: an
aligned pointer can only move into a declared-aligned slot). ~200 of ~280 lines go; the decision
becomes a pure function of the declaration specifiers shared by both passes. If (a) must stay
legal, minimum: decide from the direct initializer + declared alignas only and LogError the
later-assignment forms. (3) OPEN DIVERGENCE found while probing: `unique T*` on a function
RETURN type is NOT desugared (`unique R* mk()` -> `define ptr @_mk_RPtr__`; `unique<R> mk()` ->
`%unique__R`), so `unique R* m = mk();` reports "from borrowed value 'mk'" while the `unique<R>`
spelling runs; only Test/test_move.cb ~9367 pins the raw reading. Needs a ruling: desugar
return types too, or document the raw-pointer reading. (4) Highest merge risk: unique<T>'s
destructor does scalar delete, so any unpinned path from `new T[n]` to a unique<T> value is a
silent wrong-deallocator, not a diagnostic; the `move T*`-parameter leg is still open
(internal/issue/p2/unique-field-heap-array-through-move-param.md). Do not merge before the
structural decision.
MAINTAINER RULINGS (2026-09-02): (2) filed, not a merge blocker -
internal/issue/p2/unique-builtin-remnant-decision-is-parse-tree-lookahead.md. (3) fixed: return
types desugar in both passes (FunctionDefinition + InterfaceMethod parent, extern excluded), raw
owning returns adopt through the unique<T> ctor, borrowed sources LogError; test_move.cb ~9367
helper is now `move Resource*`; verified 764/0/8 + examples 45/0. Generic-signature gap filed as
internal/issue/p3/unique-pointer-spelling-with-unbound-type-parameter.md. (4) measured: `move T*`
already carries a hidden raw-array count; findings + a runtime-check recommendation recorded in
the p2 issue, plus a new p2 for `delete p` on a counted parameter.

BUILTIN REMNANT REMOVED (2026-09-03, worktree unique-type-prototype-4ba544). The R2/R3
parse-tree-lookahead remnant decision is GONE, and with it
internal/issue/p2/unique-builtin-remnant-decision-is-parse-tree-lookahead.md (deleted).
What landed, in order:

- **Generic value-parameter DEFAULTS** (`struct S<T, int N = <const-expr>>`), a prerequisite -
  see the Stage 6 as-built note in internal/plan/generic-value-parameters.md. Only trailing
  value parameters; a spelled default is stripped from the mangled name, so `unique<T>` keeps
  its exact old symbol.
- **`unique<T, int ALIGN = 0>`** in `cflat/core/unique.cb`. The destructor and `reset()` take
  an `if const (ALIGN > 16)` arm that frees through `__delete_aligned` (routed by declaring the
  local that holds the pointer `alignas(0, ALIGN)`; `ParseDeleteExpression` now reads the
  operand's DECLARED clause as well as its tracked block alignment, mirroring what
  `EmitOwningPtrCleanup` has always done). `sizeof(unique<T>)` is still 8.
- **Desugar** in BOTH `ParseDeclarationSpecifiers` copies: plain `unique T*` -> `unique<T>`,
  `alignas(0, N) unique T*` -> `unique<T, N>`. FIELDS and GLOBALS with an allocation-alignment
  clause deliberately stay on the builtin pointer representation (their synthesized teardown
  reads the alignment off the field's own type) - that path is untouched.
- **Deleted** from `MainListener_Declarations.cpp`: `hasRawNewArrayValue`,
  `hasAlignedMoveSource`, `hasLaterBuiltinUniqueAssignment`, `preservesBuiltinUnique`, the
  `enclosingFunction` lookup they shared, and the un-desugar application block (~295 lines);
  plus the locals-only "core unique wrapper owns a counted `new T[n]`" block. From
  `ForwardRefScanner.cpp`: the presence-only alignas veto and the R2 copy.
- **New diagnostic** (locals join the field/param/return legs): *cannot store a heap array in
  unique local 'p': unique<T> does not own arrays - use 'array<T>'*, fired from the
  declaration initializer and from a later `=`. Both use `RawNewArrayValueOf`, a ~30-line
  DIAGNOSTIC-only walk of the value side (`?:` / `??` arms included) - it decides nothing about
  representation, which is what the advisor objected to.
- **Tests**: `Test/test_core.cb` `runRawCount*` and the local-alignas accept set now hold their
  counted arrays in raw `T*` locals released through the existing `move T*` sinks, so every
  dtor-count/order assertion is unchanged in value and in intent (one leg lost its
  `values == nullptr` observation, which "raw count direct move source null" already pins).
  `Test/test_move.cb::desugaredUniqueAlignedOrder` now moves into a same-clause destination,
  because `unique<Resource, 64>` and `unique<Resource>` are different types.
  `Test/errors/err_unique_array_view.cb` gained the local decl-init, later-assignment, joined-arm
  and aligned-array legs; `err_align_alloc_mismatch.cb` / `err_move.cb` moved their array legs
  onto raw pointers. New `Test/errors/err_generic_value_default.cb`.
- Verified on macOS: `bash test.sh Release` 768/0/8, `./test_example.sh` 45/0.

**Stage 1 - close the moved-receiver dataflow gap. DONE 2026-08-31, uncommitted in worktree
unique-type-prototype-4ba544.** Shared `MainListener::CheckMovedReceiver` helper (factored
from the duplicated check in `LoadNamedVariableImpl`, `MainListener_Expressions.cpp` ~6560)
now also fires for `operator->` (`MainListener_PostfixExpression.cpp` ~809), `operator[]`
(~2236), and method-call receivers (~4635). All three scratch misuse probes are compile
errors ("use of moved variable 'x'" at the receiver use), all controls still pass, new
`Test/errors/err_moved_receiver.cb` covers method-call and operator receivers.
Independently verified: `bash test.sh Release` 750/0/8, `bash test_example.sh` 45/0.
(NOTE: suite baseline grew since the 2026-07-20 numbers above; 750/45 is the current bar.)
Original scope statement follows.
Extend the existing use-after-move check (RecordMoveUse / MovedUseSubject, precedent: the
closure-call check at `MainListener_PostfixExpression.cpp` ~3848) to method/operator calls
whose RECEIVER is a moved-from owning-struct local. Scope: ALL owning value types, not
blessed names - this is the balance rule's first test. New `Test/errors/err_moved_receiver.cb`.
Exit criteria: the three scratch misuse probes become compile errors; suite stays at baseline.

**Stage 2 - land `core/unique.cb`. DONE 2026-08-31, uncommitted in worktree unique-stage2
(branch claude/unique-stage2-core-type, off master - does NOT contain Stage 1).**
Landed exactly as designed: `cflat/core/unique.cb` (new), +123 lines of positive legs in
`Test/test_move.cb`, new `Test/errors/err_unique_type_pointer.cb`. Zero compiler changes.
Rulings made in-flight: `unique<int>` (primitive pointee) is ALLOWED - `new int` works and
the dtor path is sound, matching C++ unique_ptr<int>. Poison caveat: the dtor
`compile_error` fires only when a function instantiating the type is EMITTED, so the error
test calls the misuse function from main; a declared-but-never-called misuse compiles clean
(consistent with core's dead-if-const convention, but weaker than a true instantiation-time
poison - revisit in Stage 3 if blessing should harden it).
Independently verified: err test PASS, native test_move 2114/2114, `bash test.sh Release`
750/0/8, examples 45/0. Original scope statement follows.
`struct unique<T>` where T is the POINTEE type; `_p` is `T*` (C++ unique_ptr shape, matches
the validated prototype). API: `unique()`, `unique(move T* p)`, dtor, `operator->`, `get()`,
`release()` (move-returns `T*`), `reset(move T* p)` (frees old), `valid()`, `operator!`.
Poison bad instantiations with `if const` + `compile_error`: T itself a pointer ("unique<T*>
- T is the pointee; write unique<T>"), T a primitive. Positive legs extend an existing
`Test/test_*.cb` (no new test files); negative legs -> `Test/errors/err_unique_type_*.cb`.
OPEN SUB-QUESTION (needs ruling before this stage builds): `unique<IShape>` for interface
values. An interface value is a fat pointer, not a `T*`, so the struct needs an
`if const (is_interface(T))` arm holding the fat pointer and releasing through the vtable
dtor slot - or interface ownership stays on the builtin qualifier until Stage 4 and migrates
with the sugar. RECOMMENDATION: defer to Stage 4; do not block the scalar case on it.

**Stage 3 - diagnostic blessing. DONE 2026-09-01 (commit 3530fdd on the branch below).**
Landed: `IsCoreUniqueType` / `DisplayNameOfCoreUniqueType` on LLVMBackend (blessed = mangled
`unique__*` instantiation registered in dataStructures with core's `unique` generic template
present); call-arg diagnostics in `LLVMBackend_Overloads.cpp` render `unique<Resource>`
instead of `unique__Resource`; and ONE narrow semantic guard - a borrowed pointer bound to
the `move T*` parameter of blessed unique<T>'s CONSTRUCTOR or reset() errors with
"cannot initialize/reset unique<Resource> from a borrowed value - the source still owns it;
use 'new', a 'move' expression, or a move-returning call". The guard is explicitly
TEMPORARY: the general move-sink borrow rule stays DEFERRED per the 2026-08-10 ruling in
`internal/issue/p1/move-of-borrow-into-move-sink-parameter.md`; when that lands, this
blessing collapses into it. Guard polarity: indeterminate ownership REJECTS (safe direction
for an owning sink); non-firing sources (`new`, `move owned`, move-returning call incl.
release(), nullptr) pinned by positive legs in Test/test_move.cb. New twin tests:
`Test/errors/err_unique_type_borrow_init.cb`, `err_unique_type_stack_addr.cb`.
Independently verified: suites 756/0/8, examples 45/0, LSP all pass.
NOT done in this stage (still open for a later pass): the remaining ~30 semantic-message
twins from the audit below - only the borrowed-init/reset and callee-name families landed;
the poison-is-emission-time caveat from Stage 2 was left as is. Original scope follows.
The compiler learns to recognize core's `unique<T>` by name for ERROR TEXT: use-after-move
on a `unique<T>` local says "unique variable 'x' was moved ..." style wording matching
today's messages; init/assign paths that today say "cannot initialize unique 'b' from a
borrowed value..." get equivalent wording when the target is `unique<T>` (the ctor signature
already REJECTS the borrowed init - this stage only upgrades the resulting
no-matching-overload error to the helpful sentence). Mechanism: a small predicate
(IsCoreUniqueType: struct named `unique`, defined in core) consulted ONLY at LogError sites.
Audit the existing `err_unique_*` error tests as the checklist of messages worth preserving;
each preserved message gets a twin error test against `unique<T>`.

AUDIT DONE 2026-08-31: 31 `err_unique*.cb` files + ~20 more mentioning unique/move/owner.
Split: ~14 distinct PARSE-POSITION messages (illegal-position family in
`MainListener_Declarations.cpp:50-52,187-189,645,2826-2841`, explicit-generic-arg rejection
`MainListener_PostfixExpression.cpp:1824`, interface/impl field agreement
`LLVMBackend_Interfaces.cpp:958-967`) keep firing verbatim from the Stage 4 sugar. ~30+
SEMANTIC messages (borrow-into-unique `MainListener_Declarations.cpp:5548,5869-5881`,
alias/temp-escape families `:5766-5907` + `LLVMBackend_OwnershipTemps.cpp:456-507,2625-2660`,
use-after-move `MainListener_PostfixExpression.cpp:3862` + loop variant
`LLVMBackend_MoveDataflow.cpp:790`, delete rules `MainListener_Expressions.cpp:10666-10798`,
destruction cycle `LLVMBackend_VariablesAndIR.cpp:1229`) must be re-derived; their wording is
pinned by the tests and is the Stage 3 work list.

**Stage 4b design note (2026-09-01, written before implementation).** The desugar is NOT a
pure type rewrite; the builtin qualifier grants pointer-flavoured expression forms that the
struct type must be adapted to at each site. Adaptation-point table (builtin spelling ->
desugared emission):

| Site | Builtin `unique X* p` | Desugared `unique<X> p` |
|---|---|---|
| init from owning rvalue | `= new X()` / move-returning call | ctor call `unique<X>(<expr>)` |
| init nullptr/none | `= nullptr` | `= default` |
| reassign | `p = <owning expr>` (frees old) | `p.reset(<expr>)` |
| null test | `p == nullptr` / `!= nullptr` | `!p.valid()` / `p.valid()` |
| deref | `p->f` | already works (`operator->`) |
| borrow as plain arg | pass `p` to `X*` param | pass `p.get()` |
| explicit move to raw sink | `take(move p)` with `move X*` param | `take(p.release())` |
| move to unique<X> sink | n/a (new) | `move p` (owning struct move, Stage 1 guards) |
| delete p | rejected (err_unique_*) | struct local delete already rejected; wording via blessing |
| generic arg | `list<unique X*>` | `list<unique<X>>` (element now an owning VALUE) |
| interface | `unique IShape` | `unique<IShape>` (4a arm) |

The desugar lives in BOTH ParseDeclarationSpecifiers copies for the TYPE, and the
adaptation points live in the declaration/assignment/argument emission paths, keyed on the
destination being blessed unique<T> (Stage 3 predicate) - NOT on the keyword, so
hand-written unique<X> gets identical treatment; the keyword is pure spelling.
Phasing (each slice suite-green): (1) locals + the expression adaptations; (2) params;
(3) fields; (4) generic args + container spellings in core; (5) interface spelling; then
retarget err_unique_* expectations per slice as they flip.

**Stage 4 - keyword desugar (`unique T*` -> `unique<T>`). THE BREAKING CHANGE.**
In BOTH ParseDeclarationSpecifiers copies (ForwardRefScanner + MainListener), rewrite the
soft-keyword qualifier to the generic type before any IsUnique flag is set: `unique T*`
field/local/param/generic-arg positions all produce `unique<T>`; `unique IShape` produces
the interface arm decided in Stage 2's sub-question. The keyword remains the SUGARED
spelling users write; the parse-position rules (illegal on value types, return position,
tuple element - `doc/LANGUAGE.md` table) keep firing at parse time with today's messages,
which is exactly the "helpful errors" the sugar preserves. Consequences to handle in the
SAME change: mangling (`unique_` token -> `unique__T` instantiation names; whole-program
compiler, no ABI concern, but `--symbol-dump` output and LSP hover text change - update
LSP tests); `list<unique T*>` -> `list<unique<T>>` meaning containers now hold an owning
VALUE element, which the container migration of 2026-07-20 already made the semantics for;
re-point the `err_unique_*` suite at the desugared form. `--init` serializer rule applies to
any flag that changes meaning here.

**Stage 5 removal map (surveyed 2026-09-01, read-only inventory).** Removal order, least
hazard first: (1) IsUniqueFieldAlias (12 sites, NOT serialized); (2) re-point `is_unique(T)`
to answer true for blessed `unique<...>` instantiations BEFORE any mangling change (14
if const arms in core depend on it; it currently reads the un-stripped substitution
string); (3) kUniqueQualifierPrefix + `unique_` mangling token (+ --symbol-dump/LSP sweep);
(4) IsUniqueTypeArg + ElementOwningUnique + IsBorrowOfUniqueElement (cache keys
"unt"/"eou"/"bue" out in the same commit); (5) synthesized-dtor unique arms, re-keying
TypeOwnsUniquePointer off "has non-trivial dtor" (it also gates memberwise copy() - deleting
it naively re-enables shallow copies = double free); (6) IsUnique last (~86 sites, cache key
"uq"); (7) the five qualifier text-match sites in both ParseDeclarationSpecifiers copies,
both together, last of all. NEVER remove: IsOwningSink (cache "osk" x2) and the general
IsOwning/MoveDataflow machinery - unique<T> rides them.
MAP CORRECTIONS (2026-09-01 afternoon, removal-readiness audit with instrumentation):
(1) IsUniqueFieldAlias is NOT removable - 1d8239c made it the carrier for the DESUGARED
field-alias path too (MainListener_PostfixExpression.cpp:1674-1677); at most rename it once
the builtin legs die. (2) DONE as 1d32664 (is_unique answers IsCoreUniqueType directly;
LegacyUniqueSubstitutionSpelling survives only for is_copyable's pointer/interface split).
(3) blocked until the residual builtin spellings desugar: ForwardRefScanner interface
type-args (scan-time IsInterfaceType false -> one-sided FR/ML disagreement; the codegen
prefix producer fires zero times), using-alias bases, fixed-array `unique T* f[N]` fields,
alignas'd unique fields. Sweep already done clean: nothing in --symbol-dump/LSP/doc depends
on the prefix or token. There are SIX builtin void* handle fields, not five: the map missed
barrier.cb:47.
unique void* RESOLVED (probed; scratch/unique_void_resolution.md): the six handle fields
never migrate to unique<void> - they are mutable slots released via os.*_destroy(&field),
and `unique` only buys copy suppression; all owners have hand-written dtors. In slice 5,
re-key copy suppression to has-non-trivial-dtor FIRST, then drop the fields to plain
`void*` in the SAME slice - never before the re-key. Core generic-arg migrations: ui_test.cb:409,
ui_native/cocoa.cb:2626,5168, ui_native/win32.cb:4460,7264.

**Stage 5 - retire the builtin plumbing.**
With all spellings desugared, `IsUnique` / `IsUniqueTypeArg` / `IsUniqueFieldAlias` /
`kUniqueQualifierPrefix` mangling and the unique-specific arms of the synthesized destructor
become dead: `unique<T>` has a REAL dtor, so the ordinary owning-field destruction path
(Rule of Zero) covers what the special arms did. Remove in dependency order, suite-green at
every step; any TypeAndValue/StructData field removed must come out of the LLVMBackend.cpp
cache round-trip in the same change. The ~1600-site inventory concentrates in
MainListener_Expressions.cpp (334), MainListener_Declarations.cpp (284), LLVMBackend.h (209),
LLVMBackend_OwnershipTemps.cpp (173) - expect this stage to be several sessions and to
shrink, not grow, each file it touches.

**Risks / watch list.**
- Stage 4 changes monomorphization names; anything keyed on the mangled string (symbol
  index, caches, `--symbol-dump` selectors) needs a sweep.
- The p1/p2 open issues in this area (temp-unique-field-escapes, move-of-borrow-into-move-
  sink, deref-of-moved-pointer-guard-inside-callee) should be re-triaged after Stage 4 -
  some may become library-level non-issues, others may need re-filing against `unique<T>`.
- `dictionary<K, unique V*>` etc. in core migrate spelling in Stage 4; behaviour must not
  change (the 2026-07-20 container rule already defines it).
- Keep the builtin fully working through Stages 1-3; never ship a state where neither
  spelling has the guardrails.

### NEXT - in priority order

1. **Core FIELD migration - CANCELLED 2026-07-20. There are ZERO migratable fields, and
   `unique` FIELDS are an APPLICATION-LEVEL feature, not a core-library one.** A field is
   migratable only if a hand-written destructor frees it with a plain scalar `delete field;`;
   core has none. Every owning pointer field in core releases some OTHER way - `free()`,
   `delete[]`, an OS or allocator call, a refcount, or a chain walk - and `unique` expresses
   none of those. The old "~34 verified-owning fields" figure came from a looser criterion.
   Confirmed by measurement, not just census: the scalar-substitution fix (item 3) moved
   nothing in 635 tests, because core has no field of that shape and structurally wants none.
   **Do not revive this as a sweep, and stop looking for core consumers.**

2. **Remove `alias` as a generic type ARGUMENT - DONE 2026-07-20.** See the ledger row.

3. **`unique` under generic substitution - DONE 2026-07-20 for arrays AND scalars; ONE gap left.**
   The synthesized destructor now releases a `unique` fixed-array FIELD element by element, and
   (second pass, same day) a `unique` SCALAR field reached through substitution.

   **The scalar half, and the maintainer's ruling that drove it.** Ruling: *"`unique` is a mimic
   of C++ `unique_ptr`, so the default destructor should enumerate all fields for `IsUnique` and
   delete them, null-checked."* Executed by widening the scalar arm in `GetOrCreateFullDestructor`
   (`LLVMBackend.h` ~3079) from `f.IsUnique` to `(f.IsUnique || f.IsUniqueTypeArg)`, matching the
   array arm below it. Before: `struct Holder<T> { T _v; }` as `Holder<unique C*>` was released by
   NOTHING - measured `dtor=0 leaks=1`. After: `dtor=1 leaks=0`.

   **Blast radius was zero, and that is a weaker result than it looks.** All three suites were
   identical to baseline (448/0/8, 35/0, 152/0) because NO core type has a scalar
   substituted-`unique` field - core stores values in node/bucket ARRAY fields, already covered by
   the array arm. So the suite proved no regression but could not prove the new arm works. Both
   directions are now pinned by `Test/test_collection_leaks.cb` (`LeakGenSlot<T>`): the `unique`
   instantiation must free exactly once, and the NON-unique instantiation (`LeakGenSlot<PtrLeak*>`)
   must free NOTHING - the second leg is the important one, since freeing borrows was the whole
   risk of widening the predicate.

   **No double-free in core, for a reason worth keeping.** Hand-written teardown frees via
   `_freeValue(move ...)`, and explicit `move` NULLS the source; the synthesized delete is
   null-checked, so it degrades to a no-op. This is not luck - the wrapper runs the user dtor
   FIRST precisely so hand-written free-and-null logic wins (see "Rules that still bind").

   **The scalar `unique IFace` gap - CLOSED 2026-07-20.** The widened guard required `f.Pointer`,
   and a boxed interface value is a `{i8*,i8*}` fat pointer, so `Holder<unique IShape>` measured
   `dtor=0 leaks=1`. Closed by giving the scalar arm its own interface branch
   (`f.IsInterface && !f.IsInterfacePointer`) routing to `EmitUniqueInterfaceFieldRelease`, which
   retargets the builder at the wrapper body and reuses `EmitOwningInterfaceCleanup` (vtable dtor
   slot + `operator delete`) - the same release the array arm gives each element. Both spellings
   now measure `dtor=1 leaks=0`. With the destructor able to express it, `ValidateUniqueField`
   was relaxed for the interface-VALUE case, so a WRITTEN `unique IShape x;` field is now legal
   in both scalar and fixed-array shape. `Test/errors/err_unique_fixed_array.cb` swapped its
   now-stale interface rejection for a still-valid double-indirection one. Issue file deleted.

4. **Teardown gaps in core - DONE 2026-07-20, with two documented SKIPS.** The RAII ruling
   below was executed. Six types went RAII, two were skipped with filed evidence. See the
   ledger row and the "RAII migration outcome" table immediately after this item.

   Original item text kept below for the reasoning trail.

   **Teardown gaps in core, found 2026-07-20 by the field-migration survey.** These are the
   survey's real yield and are INDEPENDENT of the ownership workstream. All share one shape:
   core types with a MANUAL lifecycle (`init` / `start` / `destroy`) and no destructor, where
   the manual call is easy to miss or to double up. **Worth treating as ONE design question -
   should these types be RAII or stay manual - rather than four separate patches.**

   - `internal/issue/rwlock-os-state-never-destroyed.md` - `rwlock.destroy()` has ZERO callers
     tree-wide; every POSIX instance leaks its pthread state. Invisible on Windows (inline
     SRWLOCK), which is why it was never noticed. Independently re-verified.
   - `internal/issue/threadpool-continuation-ctx-leak-on-drop.md` - `threadpool.cb:861-871`
     drops `cont.ctx` on the queue-saturation path. The most concrete leak found.
   - `internal/issue/unguarded-double-init-leaks.md` - `stream.init()` and `Thread.start()` have
     no re-entry guard. `Thread.start()` twice also ABANDONS a running thread that can then
     never be joined - a correctness bug, not just a leak.
   - Not filed separately, recorded in the survey: `block_pool` / `arena_channel` / `page_pool`
     have no destructors and require manual `destroy()`; `event.destroy()` lacks the null check
     its `semaphore.destroy()` counterpart has; `ui_native/win32.cb:509` `Window.tooltip` has no
     teardown call (Windows-only, unverifiable from macOS).

   NOT bugs, recorded so they are not "rediscovered": `NumaThread._pkt` leaks by design on POSIX
   detach/kill; `stop_token._state` is a deliberate shared alias that any shape-based scan would
   misclassify as owning - migrating it would be actively wrong.

   **RULED 2026-07-20 - go RAII, matching the C++ model.** Add destructors; make `destroy()`
   idempotent (null the slot, null-check in the destructor) so the one existing manual call
   (`numa.cb:261`) does not become a double release.

   **The copy-suppression problem, and the interim answer.** C++ RAII is safe because these
   types are non-copyable by the type system (`std::mutex` deletes copy AND move). cflat's only
   non-copyability mechanism is `unique`, which requires a single-indirection pointer and emits
   `delete` - it cannot express `void* _lock` released by `os.rwlock_destroy()`. Maintainer's
   ruling: the RIGHT long-term answer is extending `unique` to cover non-`delete` releases
   (giving teardown and copy suppression from one mechanism), **but that choice is NOT being
   made yet.** For now use DISCIPLINE: audit each type for any copy path (passed by value,
   stored in a copying container, returned by value); if none exists, add the destructor and
   document that the type must not be copied. **If a copy path DOES exist for a type, do not add
   its destructor - report it instead.** A destructor on a copyable value struct double-destroys
   one OS resource per copy.

   **Side-barred design note (2026-07-20) - copyable/movable are ORTHOGONAL, and some core types
   are address-PINNED, not just non-copyable.** When the extension is eventually designed, "non-
   copyable" is not enough. `move` in cflat is a blind relocate-and-zero with NO fixup hook (there
   is no move constructor), so any type referenced by its own address is broken by a `move` even
   with no copy: `ThreadPool` stores interior pointers into its own fields (`_ctx.mtx = &_lock`,
   etc., `threadpool.cb:415-428`; workers hold `wctx = &_ctx`), and lock/`barrier` owners are held
   by worker `barrier*`. These sit in the PINNED (non-movable) quadrant, orthogonal to copyability.
   So a complete design needs a `pinned`/non-movable mark INDEPENDENT of `unique` (move-only) - one
   does not imply the other. Deferred with the extension itself; recorded so it is not re-derived.
   The move-ctor/fixup-hook half of this is shared with the container-transparency goal - see
   `internal/plan/container-ownership-transparency.md` (gap 3), which pursues "container owns iff T
   owns" by making an owning VALUE's by-value param a move-sink like `unique T*`.

   **RULED 2026-07-20 - `~Thread()` with a still-running thread is an ERROR, not a guess.**
   Compile-time rejection is ideal; a runtime error is the acceptable fallback. Do NOT silently
   join (a blocking destructor can deadlock at scope exit) and do NOT silently detach (the
   thread outlives its owner and can outlive data it references). This mirrors C++ calling
   `std::terminate()` when a joinable `std::thread` is destroyed.

   **OUTCOME 2026-07-20 - the RAII migration, with its copy-path audit.** Executed on macOS;
   `test.sh Release` 448/0/8 and `test_example.sh` 35/0 both green after.

   | Type | Copy path found | Verdict |
   |---|---|---|
   | `mutex` | none | **RAII** - `~mutex()` |
   | `rwlock` | none | **RAII** - `~rwlock()` |
   | `condvar` | none (zero uses tree-wide) | **RAII** - `~condvar()` |
   | `event` | none (only `latch._ev`; `latch` never copied) | **RAII** - `~event()` + null guard in `destroy()` |
   | `semaphore` | none (only `ThreadPool._ready` + 3 locals) | **RAII** - `~semaphore()` |
   | `barrier` | none (2 locals; workers get `barrier*`) | **RAII** - `~barrier()` |
   | `Thread` | **YES** - `nw[i] = _workers[i]` in `ThreadPool.resize()`, `threadpool.cb:659` | **SKIPPED** - `internal/issue/p3/thread-cannot-go-raii.md` |
   | `block_pool` / `arena_channel` / `page_pool` | n/a - blocked on a precondition, not a copy | **SKIPPED** - `internal/issue/p3/pools-no-destructor-shutdown-ordering.md` |

   Transitive containment was audited too (`event` -> `latch`; `mutex` -> `BucketAllocator`
   -> `block_pool` -> `arena_channel`; `mutex` -> `barrier`/`stream`/`ThreadPool`/
   `ArenaAllocator`/`NumaDomain`). No copy at any level. Interface-typed use of the two
   allocators is not a copy - an interface value is a fat pointer.

   **Idempotency came free.** `os.mutex_destroy` / `rwlock_destroy` / `cond_destroy`
   (`os.cb:675-739`) already null-check AND null the slot, and `event.destroy()` /
   `semaphore.destroy()` already null their handle. So `numa.cb:261`'s explicit
   `_lock.destroy()`, `barrier.destroy()`'s `_mtx.destroy()`, and `stream`'s `~stream()`
   are all no-ops by the time the member destructor runs. No double release.

   **A safety property worth knowing: destroying a lock cannot strand a caller.**
   `os.mutex_lock` / `rwlock_*_lock` go through `ensure_mutex` / `ensure_rwlock`, which
   lazily RE-CREATE the OS object when the slot is null. So a use-after-destroy on a lock
   silently re-allocates rather than being UB. This materially de-risks the whole change -
   including the shutdown-ordering question for the registry-lock globals
   (`_ba_reg_lock`, `_ar_reg_lock`, `_g_numaRegLock`). Pinned by
   `testLockUsableAfterDestroy` in `Test/test_sync.cb`.

   **`~Thread()` is the one place the ruling itself did not survive contact.** Besides the
   copy path, core DELIBERATELY detaches a running thread from a scope-local `Thread` in
   `channel<T>.operator>>` (`channel.cb:349-350`) - every `a >> b` pipe. An
   error-on-still-running destructor aborts that path; verified experimentally (suite went
   447/1/8, isolated to `testChannelPipeSingle`). So `Thread` needs an explicit `detach()`
   in addition to copy suppression before it can be RAII. The double-`start()` guard from
   the same issue DID land.

   **Oracles used, and where HeapAudit was vacuous.** HeapAudit tracks `new` only, and every
   lock's OS state is `calloc`'d inside `os.posix` - so HeapAudit is **VACUOUS for all six
   RAII types** and was not used as evidence for them. Instead: peak-RSS over 800k
   construct/lock/scope-exit cycles (`scratch/lock_raii_probe.cb`), 1.5 MB with RAII vs
   43.8 MB for a negative control that skips the release - i.e. the oracle was proven
   non-vacuous, and 200k x 200-byte Darwin `pthread_rwlock_t` is exactly the observed gap.
   Double-release would abort under the macOS allocator, so "returns normally" is the
   idempotency assertion. For the threadpool continuation ctx HeapAudit IS valid (the ctx is
   `new`ed by the caller), but the stronger oracle is a DISTINCT per-path dtor whose counter
   identifies WHICH release ran - not a bare count.

5. **Optimisation, not a leak:** `list<string>` named-lvalue sites still deep-copy where the
   source is provably dead. Adding `move` at those call sites is a pure win with no semantic
   change. Ruled an optimisation 2026-07-19 (`live = 0` verified), so it is not urgent.

### Resolved 2026-07-20 - kept for the corrected diagnoses

**The `unique IFace` cluster (2026-07-20).** The one-bug hypothesis was WRONG: three
symptoms, three distinct root causes, two fixed. Kept because each diagnosis corrects a
filed claim that would otherwise be re-derived.

   - *`unique IFace[N]` local frees nothing at scope exit* - FIXED. Not interface-specific at
     all: `unique C*[4]` leaked identically. `EmitDestructorsForScope` had no fixed-array arm,
     so an owning array local was never walked. Added `IsOwningUniqueArray` /
     `EmitOwningUniqueArrayCleanup` (`LLVMBackend.h`), which GEPs each slot and reuses the
     scalar emitters. `IsOwning` is deliberately NOT required there - it is set from a scalar's
     single `new` source and an array has none, so `unique` on the declaration is the ownership
     statement. Moving an interface ELEMENT out now zeroes its `{i8*,i8*}` slot
     (`MainListener.h`, in the `move` arg path) or the teardown double-frees it.
   - *`t.lookup(k, &s)` on `btree<K, unique IFace>` compiles then segfaults* - FIXED, but the
     filed diagnosis was wrong. The moved-variable check fires correctly for BOTH spellings
     (verified at a6f21f4 with the btree guard off); it was never blind to fat pointers. The
     real cause: generic substitution set `IsUniqueTypeArg` on `V* out`, so a POINTER TO the
     owning location read as a `unique` SINK, and `ApplyMoveParamTransfer` nulled the caller's
     variable - the out-param then dangled. Fixed by clearing `IsUniqueTypeArg` when the
     declarator carries an explicit star (`MainListener.h`, after `hasExplicitPointer`).
     `Test/errors/err_moved_out_param_unique_{ptr,iface}.cb` pin both spellings as a pair.
   - *field-shape rule skipped under substitution* - RESOLVED 2026-07-20, and BOTH the filed
     diagnosis and the filed fix direction were wrong. The framing "the check is skipped" is
     itself wrong: `ValidateUniqueField` (`MainListener.h:6605`) is a guard on ONE consumer -
     the synthesized destructor - and under scalar substitution that consumer was not running,
     so there was nothing for the check to guard. Re-validating at monomorphization (the filed
     direction) would have rejected `btree<K, unique C*>` and `btree<K, unique IFace>`, both
     hand-written and correct. The real defect was the opposite of a missing rejection: a
     SILENT LEAK. Fixed by making the consumer run - see NEXT item 3. The referenced issue file
     `unique-field-check-skipped-on-substituted-generic-type.md` never existed on disk; the gap
     lived only in this plan's prose. Do not go looking for it.

   **A third, unrelated bug found by the same repro and fixed with it:** `IsOwningInterfaceValue`
   tested only `TypeAndValue.IsUnique`, which substitution never sets (it records
   `IsUniqueTypeArg`). So `_freeValue(move V value)` with `V = unique IFace` - an unconsumed
   owning move param relying on scope-exit teardown - freed nothing. This is why
   `btree<K, unique IFace>` leaked every value on remove AND teardown, and it is the one place
   where the fat-pointer blind spot the cluster hypothesised actually existed.

   `_placeValue`'s `compile_error` guard and `Test/errors/err_btree_unique_interface_value.cb`
   are deleted; `Test/test_collection_leaks.cb` gains a `btree<int, unique IShapeLeak>` leg
   (400 inserts, multi-level splits, removes forcing merges) plus unique-array-local legs.


**`queue` / `stack` borrow migration (2026-07-20).** The design landed as written: a member-scope
`if const` can wrap a whole method declaration, including one whose RETURN KIND varies
(`move T dequeue()` for owned, `alias T dequeue()` for borrowed, `move T` for value). Both files
were self-inconsistent before - `enqueue(move T)` claimed ownership of a pointer element while
the destructor's `!is_pointer(T)` guard never freed it, so a pointer queue leaked with no API to
prevent it (measured `LEAKS=4` at HEAD, `LEAKS=0` after). No caller in the tree used a pointer
element, so nothing inverted. A pre-existing BUFFER OVERFLOW was found and fixed with it:
`_grow()` was gated on `_size >= _capacity` while writing to `_data[_front + _size]`, so a queue
drained from the front wrote past the end (capacity 4, enqueue 4, dequeue 2, enqueue 1 -> writes
`_data[4]`). `_grow` now tests `_front + _size >= _capacity` and repacks in place when the live
count does not need doubling.

### Rules that still bind (hoisted from the completed migrations - do not re-derive)

- **A user-written destructor does NOT suppress synthesized field teardown - they COMPOSE.**
  `GetOrCreateFullDestructor` emits a wrapper that calls the user dtor FIRST, then each field's
  release (`LLVMBackend.h` ~3123). That ordering exists so hand-written free-and-null logic runs
  before the null-checked synthesized delete, which then no-ops. Every safe interaction between
  core's hand-written teardown and the `unique` field arms rests on this. Do not "optimise" the
  ordering, and do not assume a hand-written `~T()` opts a type out of field teardown.
- **The synthesized destructor covers FIELD-SHAPED ownership only; it can never replace core's
  `is_unique(T)` release code.** It releases a scalar `unique T*` (one pointee) or `unique T* f[N]`
  (N known at compile time). Core's containers own HEAP BUFFERS OF RUNTIME LENGTH - `~list()`
  walks `_size` elements out of `_data` - and no field-shape rule can express that. Of the 32
  `is_unique()` sites in core, most are not in a destructor at all (`_placeAt` decides move vs
  copy; `_releaseAt`, `set`, `remove`, `clear` and `add`'s duplicate refusal all run mid-life,
  while the container is very much alive). Asked directly whether the widened destructor makes
  them redundant: it does not, and removing any of them reintroduces a leak.
- **`&&` and `||` do NOT const-fold** (`internal/issue/if-const-no-constant-folding-path.md`).
  Every compile-time condition MUST be a chained `else if const`. This is why `is_interface(T)`
  had to exist at all - "pointer AND NOT interface" cannot otherwise be spelled.
- **Declare the PLAIN overload BEFORE the `move` one.** Interface conformance matches the FIRST
  declared overload (`internal/issue/interface-conformance-matches-first-overload.md`), and the
  diagnostic on failure is confident and points at the wrong fix.
- **The `if const (!is_pointer(T))` guard on a transfer overload is DESIGN, not a workaround.**
  A borrowing container must not accept ownership of a pointer element - nothing would ever free
  it. For `unique T*` the plain parameter is already a synthesized move sink (D4).
- **There is no `is_alias(T)` intrinsic.** It was removed with the `alias` type ARGUMENT on
  2026-07-20 (the only thing it could ever observe was an alias-qualified type arg). The rule it
  encoded still binds in its general form: `is_pointer(T)` is true for bare, `unique` AND
  interface types, so a chained `else if const` on `is_pointer` covers every borrowed spelling.
- **`compile_error()` needs a STRING LITERAL.** A named constant silently emits garbage
  (`internal/issue/compile-error-non-literal-emits-garbage.md`), which is why the same poison
  message is repeated verbatim at several sites instead of being factored out.
- **Anything poisoned with `compile_error` MUST leave the caller a usable alternative, and the
  message must name it.** This rule exists because a poison message once told callers to write
  `move` at a call site that had no `move` overload.
- **VERIFICATION BAR: dtor-count AND HeapAudit oracles on LINKED binaries** (`-o out`, then RUN
  it) - never `--check`, never compile-only. Prove the oracle is NON-VACUOUS by introducing a
  deliberate leak and watching it report non-zero.
- **NEVER use call counts alone to decide WHICH overload ran.** A counting oracle cannot
  distinguish "both correct" from "both inverted" - it hid a fully inverted move-overload pair
  for hours. Assert identity or an observable effect.
- **THE BUILD DEPLOYS `cflat/core/*.cb` TO `x64/Release/core/` AND THE COMPILER READS THE
  DEPLOYED COPY.** Rebuild after editing any core `.cb` or you are testing stale code. The
  deploy COPIES but never PRUNES: a deleted core file leaves a stale artifact behind, and a
  test importing it will pass against a dead library (this happened to `list2.cb`).
- **A bare `T* p = new T();` NAMED LOCAL is itself an owning local**, auto-freed at its own scope
  exit unless explicitly moved out or obtained via an alias-returning call. This has now misled
  two separate investigations into misdiagnosing a double-free. Retrieve borrows via `.get()`.
- **`--init` serializer rule:** any new `TypeAndValue` / `StructData` / `AnnotationValue` field
  that an analysis reads MUST join the round-trip in `LLVMBackend.cpp` in the SAME change, or it
  is silently dropped on a warm cache and `expect_error` tests stop firing.
- **`alias` is pointer/interface only, and it is a PARAMETER/RETURN qualifier ONLY.** In a
  generic ARGUMENT it is rejected outright since 2026-07-20 (`list<alias T*>` -> write
  `list<T*>`). On a parameter or return it is load-bearing and unchanged: `list.get()`,
  `queue.peek()`, `hashset.add(alias T)` and the borrowed arm of `dequeue()`/`pop()` all use it.

### Ownership at function boundaries (SETTLED 2026-07-20)

Harvested from `internal/issue/borrowed-struct-unique-field-stored-into-owning-slot.md`, deleted
on closure. Repros survive as `scratch/moveborrow/*.cb` and `scratch/borrowstore/repro.cb`.
Shipped over commits `7548c7f`, `fa2e5b3` and the follow-on batch.

**The settled rules.** All are gated on the returned/stored struct transitively owning a `unique`
pointer (`TypeOwnsUniquePointer`, `LLVMBackend.h:3286`) - see the GATE note below.

| Situation | Behaviour |
|---|---|
| `move` of a borrowed POINTER param into a `unique` field | ERROR (a store into a `unique` field is a deferred `delete`) |
| `move` of a field out of a BORROWED struct param | ERROR |
| assignment from a `move` PARAM into an owning slot | transfers, matching the owned-local path |
| `return new T()` where the return type is the VALUE struct `T` | ERROR (was invalid IR) |
| `move T` value return of a borrowed struct param | ERROR (reuses the pointer form's message) |
| `return new T()` where the return type is a BARE `T*` | ERROR - use `move T*` or `alias T*` |
| value return, ALL returns yield a borrowed param | `alias` INFERRED; caller does not delete |
| value return, returns DISAGREE (borrowed on one path, owned on another) | ERROR |

**THE GATE: ownership, not copyability.** Measured (`scratch/moveborrow/p1.cb`): the identical
mixed-return function with a COPYABLE struct is completely correct, because returning by value
copies and there is no ownership to duplicate. Mixed returns stay legal for every other type.
Consequently these diagnostics must never be phrased as copyability complaints - copyability is
the gate, not the problem. Name the conflicting returns and both remedies.

**MEASURED DEAD ENDS - do not retry these.**

- **Do NOT check at the call/parameter boundary.** Rejecting a `unique`-owning struct passed by
  value cost 13 suite failures: a genuine false positive on `bool operator==(LeakHolder a,
  LeakHolder b)` (by-value passing is a SAFE read - measured `dtor=1`, not 2), plus 12
  preemptions of the six `err_*_noncopyable_*` container poisons, which are better targeted. The
  check belongs at the STORE, which is where it now lives.
- **Do NOT widen `ComputeReturnsOwned`** (`MainListener.h:684`). A previous widening caused a
  SIGABRT in `move list<T> copy()`, and carrying `CallerName` caused a false "use of moved
  variable". A `move S` VALUE return is deliberately NOT `currentFunctionReturnsOwned`; key off
  the DECLARED return type instead (`currentFunctionReturnTV.IsMove && !Pointer`). This trap
  derailed two separate attempts.
- **`alias` is VIRAL through locals.** A local bound from an `alias` return cannot be returned
  from a non-`alias` function. Migrating `json.cb`/`xml.cb` `_allocNode()` cascaded to every
  node-returning parser helper and the public `JsonParser::parse`. Semantically right, but budget
  for propagation, not a one-line edit.

**KNOWN REMAINING GAP: the indirect fresh-allocation forms still leak silently.**
`T* h = new T(); return h;` and the `unique`-local form are NOT caught; only the direct
`return new T()` is. Provenance IS reachable at the return, but widening the gate produced
**250 of 460 tests failing**, because it rejects `cflat/core/function.cb:9`
(`i8* __closure_env_alloc(i64 n) { i8* base = new i8[n]; return base; }`) and siblings that every
program imports. Closing it requires migrating core's raw-memory plumbing FIRST - `function.cb`
and `arena.cb` are the only two allocator-ish core files using the two-step form. Open design
question before anyone attempts it: if every raw-memory primitive becomes `alias`, the marker
stops signalling "I know what I am doing" at exactly the layer where ownership mistakes cost
most. Decide `move` vs `alias` per primitive rather than blanket-`alias`ing to clear the failures.

### Deferred by maintainer ruling - do not action as part of the above

- **Lifetime / dangling for borrowed containers: ignored, none planned.** A `list<T*>` outliving
  its owner is undetectable. The hazard pre-existed unnamed; borrow-by-default makes it the
  blessed pattern, which raises the stakes but does not change the risk.
- **Blocker 1 is RESOLVED 2026-07-20.** It was a reachable double free: a plain by-value param
  BORROWS an owning value while local init MOVES it, so storing that borrowed struct into an
  owning slot duplicated a `unique` pointer into two owners and aborted at teardown, with no
  diagnostic. NOT a lifetime bug - it reproduced with both values in the same scope. RULED and
  fixed as a set of missing DIAGNOSTICS plus one unrecognised owner; no borrow/move redesign was
  needed. Its issue file was deleted on closure; the durable findings are in
  "Ownership at function boundaries" below. Repros survive in `scratch/moveborrow/` and
  `scratch/borrowstore/`.
- **Standing question (positional asymmetry):** bare `Circle* c = new Circle();` is owning while
  bare `list<Circle*>` is borrowed - same spelling, opposite defaults by position. Endpoint (a),
  containers erase provenance so they force you to say it, is where the design landed. Endpoint
  (b), `unique` becomes the owning marker everywhere and bare `T*` uniformly means borrowed,
  stays on record as the possible destination; the container migration was its dress rehearsal.
- **Known gap:** a `compile_error`-poisoned method reached ONLY via virtual interface dispatch is
  not caught. No such call exists in-repo.
- **Follow-ups parked in Part II "Open questions":** borrowed views of owning lists (D6), a clone
  story for unique containers, `--sanitize` runtime data-pointer compare for fat-pointer alias
  stores, and the enumerate-as-checklist sweep of conversion sites for unique interface values.

### Polish debt

RE-VERIFIED 2026-07-20 (see Completed ledger for the fix commit-in-progress). Five of the
seven filed items were already stale (fixed in passing by later work) and reproduced clean;
two were real and fixed:
- `delete` on a unique LOCAL: STALE. Already reports "cannot delete unique local 'n' - a unique
  local is freed automatically at scope exit ...", not "unique field".
- Dead `TypeOwnsUniquePointer` bail in `GetOrCreateMemberwiseCopy`: STALE AS DESCRIBED - no such
  bail exists inside that function (never has, per `git log -S`). The only `TypeOwnsUniquePointer`
  gate is the choke point in `CreateOverloadedFunctionCall` (before the call to
  `GetOrCreateMemberwiseCopy`), and it is reachable (reproduced: copying a struct with a `unique`
  field and no user `copy()` hits it). Left unchanged.
- Trap A `unique field ''` for bare self-field access: STALE. Reproduced `delete n;` inside the
  owning struct's own method - reports "cannot delete unique field 'n' - ..." with the field name
  filled in, not empty.
- `unique int x[4]` pointer-flavored message: STALE. `int` is genuinely not a pointer type, so
  "requires a single-indirection pointer type such as 'unique Node* n'" is the correct rejection
  reason (confirmed against the ACCEPTED cases: `unique Node* kids[8]` and `unique IS one/slots[4]`
  both compile; only a non-pointer element type or double indirection still reports this message).
- Stale "frees and nulls" comment: STALE - that exact phrase does not appear anywhere in the tree
  (`grep -rn "frees and nulls"` matches only this plan doc's own polish-debt line).
- One duplicate test in `test_move.cb`: REAL, FIXED. Commit `54d6803` added
  `testPtrMoveOverloadIdentity()` (covering `probe()`'s plain/move overload pair with STRONGER
  assertions - it also checks the borrowed pointer stays readable) in the same commit that touched
  the inline `main()` "Test 8" block, leaving `main()`'s `probe(a)`/`probe(move a)` checks
  (`probe_plain_call_picks_borrow`, `probe_plain_call_freed_at_scope_exit`,
  `probe_picks_move_for_explicit_move`, `probe_move_freed_once`) a strict subset of the new
  function's coverage. Removed the redundant inline block; kept the `delegateProbe` sub-case
  (not covered elsewhere).
- `std::set` vs `std::unordered_set` inconsistency: REAL, FIXED. The move-dataflow tracking code
  (`NamedVariable::MovedFields`, `MovedStateSnapshot::movedFields`, all of `MoveDataflow.h`'s
  `MovedSet`/`visited`/`seen`) used `std::set` exclusively while the rest of the file's dedup/seen
  sets (`TypeOwnsUniquePointer`, `fullDestructorInProgress_`, etc.) use `std::unordered_set`. None
  of the `std::set` usages relied on sorted iteration (only `insert`/`count`/`erase`/`operator==`),
  so switched all of them to `std::unordered_set` to match the dominant convention.

### Open issues owned by this workstream

Filed and NOT fixed. Container work is done. The first nine are the compiler-side remainder;
the last two are the RAII migration's two documented SKIPS (see NEXT item 4). The three core
teardown gaps the field-migration survey filed (`rwlock-os-state-never-destroyed`,
`threadpool-continuation-ctx-leak-on-drop`, `unguarded-double-init-leaks`) were all FIXED on
2026-07-20 and their files deleted, as was `duplicate-add-leaks-unique-value.md`.

**The two-defects-or-one question is SETTLED: one defect.** Verified 2026-07-20 by a 2x2
matrix on linked binaries - the controlling variable is `return move r`, not the `move` return
TYPE and not a container. Both original issue files stated repros that were wrong on
load-bearing points and were consolidated into
`move-return-named-struct-local-strips-owned-bits.md`. The plan's own conjecture (ownership
flags "not surviving") was also wrong: the compiler ACTIVELY emits an `and ..., 0x7FFFFFFF` to
strip the owned bits. Note this is the one time the one-bug hypothesis HELD - the `unique
IFace` cluster went the other way, so keep testing it rather than assuming either outcome.

| File | What |
|---|---|
| `unique-iface-scalar-field-not-released.md` | scalar `unique IFace` field (written or substituted) is released by nothing - the fat-pointer half of NEXT item 3 |
| `move-return-named-struct-local-strips-owned-bits.md` | `return move <named struct local>` is misclassified as a borrow return and STRIPS the owned bits - silent leak on any hand-written `copy()` that move-returns. Consolidates two earlier files whose repros were both wrong |
| `interface-conformance-matches-first-overload.md` | conformance matches first declared overload |
| `compile-error-non-literal-emits-garbage.md` | `compile_error(CONST)` emits a stray character |
| `if-const-no-constant-folding-path.md` | `&&` / `||` do not const-fold |
| `if-const-global-condition-crash.md` | `if const` on a global condition crashes |
| `delete-borrow-via-named-local.md` | `delete` on a borrow via a named local is not rejected |
| `generic-interface-explicit-type-arg-base-clause.md` | explicit base-clause form drops `*` / `unique` |
| `thread-cannot-go-raii.md` | `Thread` is value-copied in `ThreadPool.resize()` AND deliberately detached by `channel.operator>>`; needs `detach()` + copy suppression before RAII |
| `pools-no-destructor-shutdown-ordering.md` | `arena_channel`/`block_pool`/`page_pool` teardown has a quiescence precondition a destructor cannot check |

### Completed ledger

| Done | What | Where |
|---|---|---|
| 2026-07-16 | Part I stages 1-5: `unique` FIELD qualifier, assignment discipline, Trap B, code-review remediation, interface field contract | git history |
| 2026-07-18/19 | Part II stages 1-6: interface value ownership, `unique` in generic-argument position, unique locals, synthesized move params (D4), `dictionary`/`hashset` unique support | Part II below |
| 2026-07-19 | `alias` as a generic type argument LANDED (compiler only). `IsAliasTypeArg` deliberately separate from `IsAlias`; serialized as `o["alt"]` | - |
| 2026-07-19 | Prerequisites: both move-overload defects FIXED, `is_copyable(T)` landed, `is_pointer(IShape)` flipped TRUE | - |
| 2026-07-19/20 | **`list` migrated to borrow-by-default**, `IList<T>` reshaped with no `move`, `take()` removed, six code-review findings fixed | commit `6528268` |
| 2026-07-20 | **`hashset` + `dictionary` migrated.** Fixed a compiler bug: the move/borrow tie-break ran only on the perfect-match tier, so a LITERAL key made `d.add(1, x)` silently consume `x` | commit `7f5d433` |
| 2026-07-20 | **`hpc/btree` migrated** - the last container on its own convention. Fixed a real duplicate-key bug that freed the caller's value. `_freeValue`/`_freeKey`'s move-param workaround SURVIVED and had to (its body is now EMPTY - the param's scope-exit destructor IS the release) | commit `54d6803` |
| 2026-07-20 | **`array` migrated** | commit `54d6803` |
| 2026-07-20 | **`queue` + `stack` migrated** - `_placeAt`/`_releaseAt`, plain + guarded-`move` insert, `dequeue()`/`pop()` return kind selected by member-scope `if const` (`alias` for a borrowed element), release-walk destructor. Fixed a pre-existing `queue._grow()` capacity bug: `if (_size >= _capacity)` ignored `_front`, so enqueueing after a partial drain wrote PAST the buffer | commit `54d6803` |
| 2026-07-20 | **`unique IFace` cluster: 2 of 3 FIXED** (+ a third, unrelated bug found with them). Owning fixed-array locals were never walked at scope exit (`unique C*[4]` leaked identically - never interface-specific); substitution set `IsUniqueTypeArg` on a `V* out` param so a POINTER TO the owning location read as a sink and dangled; `IsOwningInterfaceValue` tested only `IsUnique`, which substitution never sets, so `btree<K, unique IFace>` freed nothing. Unblocked `btree<K, unique IFace>` | commit `4cce536` |
| 2026-07-20 | **`alias` REMOVED as a generic type ARGUMENT** (NEXT item 2). Bare means borrow, so `list<alias T*>` was a synonym for `list<T*>`. Chose a HARD ERROR over silent acceptance so the dead spelling cannot rot. Deleted: `StripAliasQualifier`, `kAliasQualifierPrefix`, `MangleTypeArg`'s `alias_` token, `PeelTypeArgSuffix`'s alias out-param, the `is_alias(T)` intrinsic, `TypeAndValue.IsAliasTypeArg` and its `o["alt"]` cache entry. `IsAlias` (param/return) UNTOUCHED. `dictionary`/`hashset` poison messages now recommend the bare spelling | commit `4cce536` |
| 2026-07-20 | **Synthesized destructor now RELEASES a `unique` fixed-array FIELD** (NEXT item 3). `GetOrCreateFullDestructor` routes `unique T* f[N]` / `unique IFace f[N]` - written, or reached through a `unique` generic type argument (`IsUnique \|\| IsUniqueTypeArg`) - through the existing `EmitOwningUniqueArrayCleanup` walk, with the member builder and `currentFunction` temporarily retargeted at the wrapper body. `btree` took option (a): `_clearValue` now abandon-clears a `unique` slot exactly as it already did for an owning VALUE slot, so every stale split/borrow duplicate reads null and the node destructor frees each live slot once. `_freeValue`/`_freeKey` are UNCHANGED and still needed (remove/overwrite/clear happen before teardown). Declaration-time rule relaxed: `unique T* f[N]` is now accepted and the "fixed arrays are not supported yet" message is DELETED. Interface fields stay rejected in EVERY shape - scalar `unique IFace x` is refused by the same single-indirection rule, so relaxing only the array form would be incoherent; that message never blamed the removed limitation | commit `4cce536` |
| 2026-07-20 | **Core manual-lifecycle types went RAII** (NEXT item 4). `mutex`, `rwlock`, `condvar`, `event`, `semaphore`, `barrier` gained destructors after a copy-path audit found ZERO copy paths for each (including transitively, through `latch`, `stream`, `barrier`, `ThreadPool`, `NumaDomain`, and the `BucketAllocator` -> `block_pool` -> `arena_channel` chain). Idempotency was already free - every `os.*_destroy` nulls its slot - so `numa.cb:261`, `barrier.destroy()` and `~stream()` did not become double releases. `Thread` SKIPPED (real copy path + deliberate detach); pools SKIPPED (quiescence precondition). Oracle: peak RSS over 800k lock cycles, 1.5 MB vs 43.8 MB for a negative control - HeapAudit is VACUOUS here (lock state is `calloc`'d, not `new`ed) | commit `4cce536` |
| 2026-07-20 | **`stream.init()` and `Thread.start()` re-entry guards** - both now reject a second call with a diagnostic + `abort()` (the `list._checkBounds` idiom) instead of leaking prior state; `start()` also no longer abandons a running thread that could never be joined | commit `4cce536` |
| 2026-07-20 | **`return move <named struct local>` no longer strips the owned bits.** `ParseMoveExpression`'s struct-VALUE branch never set `lastOwningResult`, so the return site classified it as a BORROW and emitted `and i32 %x, 0x7FFFFFFF`; the caller got a non-owning struct, its destructor found nothing, and every owning field (e.g. a `string`) leaked. Hit any hand-written `copy()` that move-returns, with OR without a `move` return type, container or not. Setting `lastOwningResult = true` in that branch is the whole fix - it also drops the stray `dtorfull` on the zeroed source. Two suggested extras were REJECTED after measurement: carrying `CallerName` re-marks the origin in the move tracker (false 'use of moved variable'), and widening `ComputeReturnsOwned` to accept a `move` struct-VALUE return type DOUBLE-FREES (`move list<T> copy()` results become owned temps while the receiving local also destructs) | working tree |
| 2026-07-20 | **`threadpool` continuation-drop leak FIXED, plus a hang found with it.** The queue-saturation path now releases `cont.ctx` through `__threadpool_drop_ctx` outside the pool lock. Required a new `TaskHandle._contDtor` field: `then()` was silently DISCARDING the caller's `ctxDtor` when the continuation was deferred, so the drop path could not have used the right release. Companion bug fixed on the same branch: the drop never published `done` on the continuation's handle, so any caller holding it spun forever in `~TaskHandle`. Both pinned by `testContinuationDroppedOnSaturation` (`Test/test_threadpool.cb`), verified non-vacuous | commit `4cce536` |
| 2026-07-20 | **`dictionary<K, unique V*>.add()` duplicate-key leak FIXED.** Under borrow-by-default `add(alias K key, V value)` takes a plain parameter that IS a synthesized move sink once `V` is `unique`, so the bare `return false;` on the duplicate path dropped an already-transferred value. Added `_freeValue(move V value)` with an EMPTY body (the move param's own scope-exit destructor is the release - `btree.cb:244`'s pattern) called under `if const (is_unique(V))`. Measured `dtor=1 leaks=1` before, `dtor=2 leaks=0` after. `hashset` audited and NOT affected: `_slot()` poisons any unique-element instantiation with `compile_error`, so `hashset<unique T*>` never compiles. Issue file deleted | UNCOMMITTED (staged) |
| 2026-07-20 | **Synthesized destructor's SCALAR arm widened to `IsUnique \|\| IsUniqueTypeArg`** (NEXT item 3, second pass). A scalar field made `unique` purely by substitution was released by nothing: `Holder<unique C*>` measured `dtor=0 leaks=1`, now `dtor=1 leaks=0`. All three suites unchanged from baseline (448/0/8, 35/0, 152/0) because core has NO such field - the benefit is application-level generics, so the suite could not pin it and `Test/test_collection_leaks.cb` gained `LeakGenSlot<T>` legs in both the `unique` and the borrow direction. Scalar `unique IFace` remains unreleased (fat pointer fails the `f.Pointer` guard) and is filed separately | UNCOMMITTED |
| 2026-07-20 | **Interface array FIELD sizing bug FIXED** - `IFace v[N]` allocated ONE element with a correct 16-byte stride, silently clobbering everything after it. Plus a second bug found while verifying: the subscript handler never refreshed `interfaceVar`, so `a[i].method()` dispatched off the stale base address (affects LOCALS too) | commit `4cce536` |

---

# Part I - the `unique` field qualifier

Created 2026-07-16. **Stages 1-5 DONE (2026-07-16).** Both open decisions settled
(2026-07-16): the keyword is `unique`, and recursive chains are rejected. See Decisions.

Complements [ownership-move-alias-discipline](ownership-move-alias-discipline.md), which
stays correct for *parameters*; this part addresses *fields*, which that note does not cover
(and see the Appendix - that note has three factual errors worth fixing).

Error when a `unique` field's pointee type transitively reaches a `unique` field of the same
type. Transitive, not a `field type == my type` name test: `A` owning a `unique B*` while
`B` owns a `unique A*` is the same cycle one hop longer.

**Why: there is no good answer in the general case.** Both mechanisms the compiler could
synthesize are defective, in ways that depend on data the compiler cannot see:

- **Recursive teardown** (the natural synthesis) costs one stack frame per link on a chain,
  and one per level on a tree. A balanced tree is ~5-20 frames and fine; a sorted-insert BST
  or a long `next` chain is N frames and overflows during teardown, far from the code that
  built it. C++ has exactly this bug with `unique_ptr` and has never fixed it.
- **Iterative teardown** (worklist instead of self-call) is correct at any depth, but needs a
  heap worklist allocated *during destruction*, which can itself fail under memory pressure.

The shape that is safe is a property of the data, not the type - and the author is the only
one who knows it. So the author writes the destructor.

The chain-vs-tree distinction is what makes this non-obvious and is worth recording: the
original argument for rejecting used only `unique JsonNode* next` (a chain, N frames), which
made rejection look free. It is not free - it also rejects trees (`btree_node.children[17]`,
`btree.cb:77`, whose pointee is `btree_node<K,V>` itself), which recurse only per level.
Rejection was briefly reversed on that basis and then re-confirmed: allowing trees means
allowing degenerate trees, and there is no sound static test separating them. Heuristics
considered and rejected: "reject 1 self-pointer, allow 2+" (a skewed binary tree is linear
too; a 3-element chain is harmless).

**Consequences, accepted:**

- `btree_node` keeps its hand-written destructor.
- `json.cb:208` / `xml.cb:211` keep theirs - they opt out over the arena/heap duality anyway.
- cflat still accepts a hand-written `~JsonNode() { delete next; }`, which recurses
  identically. We decline to *synthesize* a footgun the author may still write deliberately,
  knowing their data. The line is that synthesized code should never need auditing.

## What Part I does not do

- ~~Field-only~~ **SUPERSEDED by Part II:** `unique` is now legal on locals, params
  (synthesized move), and generic type arguments (`list<unique Node*>`). Return position
  remains unsupported (transfer out is `move`, borrow out is `alias`).
- **No lifetimes.** Like C++, a borrow can still outlive its owner. Trap A is caught only in
  the callee, at a field store from a borrowed parameter. Rust's guarantee needs the escape
  analysis the discipline note rules out permanently.
- **Does not touch the value-type path.** `string` keeps its runtime owned bit (`_len` high
  bit, `LLVMBackend.h:2613`, `:2620-2627`). `unique` is a static, compile-time property. The
  two mechanisms stay separate and cover different things.

# Part II - interface value ownership and `unique` in generic-argument position

Promoted 2026-07-18 from `internal/issue/list-interface-element-copy-imperfect.md` (deleted).
Promoted because the container symptom is not fixable in the container - it needs a
language-level answer to what an interface value owns.

### Settled decisions (ballot, ruled 2026-07-18)

- **D1 - positions.** `unique` legal in any declaration position (locals, fields, type
  arguments), only on pointer or interface types. `unique int` / `unique Circle` (value
  struct) = compile error "unique requires a pointer or interface type".
- **D2 - whole-type qualifier (user ruling).** `unique` applies to the ENTIRE declared
  pointer type - `unique Circle**` is one owning value that is, at the end, still a pointer.
  No per-star binding. `is_unique` = leading `"unique "`, `is_pointer` = trailing `*`; both
  hold on `unique Circle**`.
- **D3 - signatures (amended after stage 3).** A `unique`-typed PARAM is legal and is exactly
  a synthesized move param (D4) - the two spellings are defined equivalent, so they cannot
  drift. `unique` in RETURN position stays unsupported; transfer out is `move`, borrowing out
  is `alias`. (Original D3 forbade params too; D4 superseded that, and the two err_ tests
  asserting the old field-only restriction were deleted in stage 3.)
- **D4 - synthesized move params (user ruling).** Callee declares the ownership sink; no
  call-site `move`; `shapes.add(c);` transfers and nulls `c`. A unique value passed to a
  plain-pointer parameter is a borrow; ownership stays with the caller. (Supersedes two
  earlier ideas: blanket implicit move-on-pass AND required call-site `move`.)
- **D5 - borrows and moves through alias.** Bare local from a borrow is borrowed (no
  IsOwning). `unique X* u = l[i];` without move = error. `move l[i]` is allowed: nulls the
  slot in place, size unchanged (the std::move(v[i]) idiom); `take()` is
  remove-and-transfer.
- **D6 - no conversion** between `list<unique T*>` and `list<T*>`; distinct instantiations,
  overloads do not cross. Borrowed views of owning lists = future work.
- **D7 - `is_unique` outside a generic context** returns 0, same convention as `is_pointer`.
- **D8 - teardown + F3 dependency.** Single dtor branch with null skip; the interface leg
  needed F3 (interface null-compare). Resolved: F3 was already on master.
- **D9 - `delete` on a borrowed raw pointer** stays legal (C compat); double-free is
  `--sanitize` territory, no static diagnostic.
- **D10 - canonical text.** One normalization point yields `"unique Circle*"` (single space,
  attached star); substitution map and mangling derive from it.
- **D11 - serializer.** Any new `TypeAndValue`/`StructData` bit joins the `--init` round-trip
  in the same change (standing repo rule).
- **D12 - insertion params are plain `T`; ordinary passing semantics decide (user ruling
  2026-07-19; STRING LEG AMENDED same day - see below).** No container-specific rule:
  unique T => synthesized move (D4); owning value struct => auto-move (existing language
  default); primitive => copy; bare pointer/interface => borrow. Supersedes the explicit
  `move T` insertion params from Stage 5/6 and resolves the borrowed-list `add` leak trap.
  **AMENDMENT (premise error, corrected 2026-07-19):** the ruling as first recorded said
  `string` => COPY, "consistent with `string b = a`". That premise is FALSE - `string b = a`
  MOVES (verified: use-after-move on the next read; `core/string.cb:12` documents
  "move-by-default"). The consistent semantics is therefore TRANSFER: `l.add(s)` moves and
  `l.add(s.copy())` copies, exactly as `string b = a` / `string b = a.copy()` do. With that
  correction the string leg needs no call-site `move` and no copy-by-default, which is what
  makes unblock option (c) viable. Behavior change is then confined to bare
  pointers/interfaces (consume -> borrow). See live section item 2 and Stage 7.

### copy()

Bitwise copy of unique elements is a double-free. Unique instantiations are MOVE-ONLY at
first: `copy()` on `list<unique T*>` / `list<unique IShape>` is a compile error, via the
`compile_error` builtin (deferred-poison mechanism; see git history). A later clone story
for interfaces needs a vtable copy slot every implementor fills; for pointers, an element
`.copy()` through `new`. Not in scope.

## Interaction with interface fields (F1)

Interface fields (the F1 feasibility spike, plan deleted 2026-08-31 as landed) make
interfaces more value-like. Layering rule that keeps the two independent:

- **Fields are owned by the implementor object, or by a `unique` interface value that owns
  the object - never by a borrowed view.** All field-store rules apply through the interface
  path via the `IsInterfaceField` flag (flag-based, never GEP-shape-based - the spike's
  hardest-won lesson), on BOTH store paths (assignment and `EmitOneFieldInit` brace-init).
- **Teardown is free.** The vtable fullDtor is the implementor's dtor, which already destroys
  its owned fields. Whichever container knows to invoke the dtor slot gets fields for free.
- Audit before F1 lands: the shared field-store helpers must classify "is a field store" by
  flag, not GEP shape, or interface fields leak on the brace-init path only.

## Open questions

- **Positional asymmetry** - see live section item 6.
- **Move out of an interface field** (`string s = move iface.title;`): mutation-at-a-distance
  through a view. Language philosophy is consistent with allowing it; forbidding first is the
  reversible choice. Needs a call when F1 lands.
- ~~Does the same hole exist in `dictionary<K,V>` / `hashset<T>`?~~ Assumed yes (shared
  mechanism); being closed by Stage 6, in progress.
- Alias-reject cannot see through fat pointers (two views of the same object are statically
  indistinguishable). Accepted gap - same as raw pointers today; a `--sanitize` runtime
  data-pointer compare before owned-field stores is a cheap follow-up.
- Return-position and other conversion sites for unique interface values (ternary arms,
  brace-init of interface fields, `return move`, default args) - enumerate as a checklist,
  do not fix as-found (the brace-init parity lesson).

## Related

- `internal/plan/ownership-move-alias-discipline.md` - parameter discipline
- interface fields (F1) - see "Interaction with interface fields" above; the
  feasibility spike plan was deleted 2026-08-31 as landed (doc/UI.md is the record)
- `internal/plan/ownership-sanitizer.md`, doc/LANGUAGE.md ("Local ownership";
  cflat/MoveDataflow.h) - runtime and dataflow companions to the static rules here
- `internal/issue/brace-init-field-store-not-at-parity.md` - the durable store-path-parity
  rule (shared helpers from the start), learned partly from this feature
- `Test/test_interface.cb:95-106` - both boxing forms
- `Test/test_core.cb:639-661` - the owning-list API contract
- `Test/test_collection_leaks.cb` - leak regression oracles (HeapAudit + dtor counts)
