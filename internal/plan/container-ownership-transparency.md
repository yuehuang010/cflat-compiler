# Container ownership transparency: make ownership depend on T, not on the container

Opened 2026-07-21. GOAL, multi-change, iterative. This is the forward direction that the queue
`if const` spike surfaced. It extends the `unique` workstream ([[unique-ownership]]) and relates to
[[ownership-move-alias-discipline]], [[move-dataflow]], and the delete-borrow issue
(`internal/issue/delete-borrow-via-named-local.md`).

## The goal, in one line

A core container (queue, list, stack, dictionary, ...) should be **ownership-transparent**: it owns
exactly what its element type T owns, and its per-element operations should defer to T's own
move / copy / destroy - with NO container-level `if const (is_unique(T)/is_pointer(T)/...)` policy.
Ownership stopped being a container decision when `unique` moved it into the type; the container code
should follow.

## The shift that motivates this

- BEFORE `unique`: a container CHOSE a policy and baked it in ("BARE BORROWS, unique OWNS"). Ownership
  was a property of the container. Per-element behavior was hand-dispatched with `if const`.
- AFTER `unique`: ownership lives in T - `unique T*` owns, `T*` borrows, `string`/`Holder` own a heap
  resource, `int`/`Point` own trivially. The container's owning-ness is now a FUNCTION of T. The
  container should be transparent, not a policy-holder.

## What we validated (queue spike, 2026-07-21; queue.cb currently holds the collapsed, UNSOUND form)

Collapsing the read/release side WORKED; the insert side did NOT. The split is not a coincidence -
it tracks who owns the SOURCE of the value being moved:

| queue op            | collapsed to          | source of value        | result |
|---------------------|-----------------------|------------------------|--------|
| `dequeue`           | `return move slot`    | container's own slot   | SOUND  |
| `_releaseAt`        | `move slot` (drop)    | container's own slot   | SOUND  |
| `_placeAt`/`enqueue`| `slot = move value`   | the CALLER's variable  | UNSOUND for owning values |

Moving OUT is transparent because the container controls (and can null) its own slot. Moving IN fails
at the PARAMETER BOUNDARY: a by-value `T value` param does not null the caller's source for owning
VALUE types, so caller and slot co-own one resource -> double free.

### Double-free matrix (collapsed `slot = move value`, no `if const`)

| element T                         | owns a resource? | enqueue(local) | result   |
|-----------------------------------|------------------|----------------|----------|
| `int`, `Point{int x,y}`           | no               | copies         | exit 0   |
| `unique R*`                       | yes (pointer)    | move-sink: consumes + nulls source | exit 0 |
| `string`                          | yes (heap buf)   | bitwise-alias, source NOT nulled | 134 |
| `Holder{unique R* r}`             | yes (via field)  | bitwise-alias, source NOT nulled | 134 |

The double-free hits EXACTLY the resource-owning value types. `unique R*` is also resource-owning yet
fine - because its by-value param is a move-sink (nulls the caller). That asymmetry is the whole bug.

## Root cause and the four gaps

The container code is already transparent; the LANGUAGE does not move an owning VALUE across a call
boundary correctly. IR proof (queue<string>): `enqueue` does `store %string %value -> slot` (raw
{ptr,len} copy) and `main` then frees `s` AND the slot (`~queue`) - two frees of one buffer.

Gaps between where we are and full transparency:

1. **By-value param is not a move-sink for owning VALUE types.** `ApplyMoveParamTransfer`
   (LLVMBackend.h ~10089) gates the "null the caller" transfer on `IsMove || IsUniqueTypeArg` - a
   POINTER property. It must key on "owns a resource": `IsUniqueTypeArg || IsOwningValueType(T) ||
   T == "string"`. Then `string`/`Holder` locals get consumed-and-nulled just like a `unique R*`
   local (verified: `enqueue(move p)`/`enqueue(p)` both null `p` and mark it moved-from for
   `unique R*`; `enqueue(move h)` is a silent NO-OP for `Holder` today - `h` stays readable -> 134).
2. **`move` into a plain `T value` param is silently accepted for owning values.** For a COPYABLE
   value the compiler correctly rejects it ("'move' transfers nothing, parameter borrows"); for an
   owning value it is silently accepted and miscompiled. The guard that fires for copyable values
   must also fire for owning value types (or the param must be treated as a sink, per gap 1).
3. **No move constructor.** cflat's `move` is a whole-struct blind relocate-and-zero-source; it has
   no member-wise fixup hook. For the plain owner case zeroing the source struct nulls the owned
   pointer (sufficient), but the by-value param path fails to zero the caller's source at all. C++
   reaches soundness via a member-wise move ctor (`unique_ptr`'s move ctor nulls the source). See
   the side-bar in [[unique-ownership]] (pinned types) - a move-ctor/fixup hook is the deeper item.
4. **`string` is special-cased, not modeled as owning.** `IsOwningValueType("string") == false`
   (string is not in `dataStructures`); dozens of `TypeName == "string"` special cases exist. The
   deep version of this plan gives `string` a `unique char*`-style owning buffer so it flows through
   ONE owning-value path (copy suppression, move-sink, delete checks) instead of ad-hoc casing.

## Ease of use: insert is MOVE-BY-DEFAULT (the load-bearing statement)

`enqueue("asdf")` is move-by-default (an owning rvalue is consumed with no keyword). The SAME must
hold for a named owner: `enqueue(myHolder)` MOVES `myHolder` - no `move` keyword - PROVIDED
`myHolder` is an owner/non-alias AND `enqueue`'s parameter is by-value (`T value`). This is not new
behavior: `unique R*` already does exactly this (`enqueue(p)` consumes `p` and marks it moved-from
with no keyword), and cflat assignment already does it (`string b = a` moves `a`). Extending it to
owning VALUE types is pure consistency, and it is the ergonomic default that makes the collapse worth
doing. To KEEP the source, the caller opts in at the call site: `enqueue(x.copy())`.

Dispatch is on the ARGUMENT's ownership, which the compiler knows, not on container `if const`:
- owner / non-alias (`unique T*`, `string`, `Holder`) -> MOVE (consume + null the source);
- borrow (`T*`, `alias T*`, interface value) -> ALIAS (source is NOT an owner, so NOT consumed);
- primitive -> copy (move == copy, harmless).

## This DISSOLVES the copy-vs-move fork (correction to an earlier claim)

An earlier draft said "copy vs move on insert is a genuine per-T fork that cannot collapse" - citing
C++'s two `push_back` overloads (`const T&`, `T&&`). That reasoning assumed COPY-by-default. cflat is
MOVE-by-default, so the fork DISSOLVES: insert is ALWAYS `slot = move value`, and the copyable owner's
"choice" to preserve the source becomes a call-site `.copy()`, NOT a container overload. So:

- `Holder` (non-copyable) and `string` (copyable) take the SAME insert path - `slot = move value`.
- There is NO per-element `enqueue(move T)` vs `enqueue(T)` fork; one by-value `enqueue(T value)` that
  moves suffices. (Removing the overload was only WRONG while the move-sink gap made the plain form
  double-free; once the by-value param is a move-sink for owners, one overload is correct.)
- `is_copyable(T)` survives ONLY for WHOLE-CONTAINER copy (`list.copy()` deep-clones each element) -
  never for per-element insert.

C++ carries the dispatch in T's special members (copy/move ctor, some deleted) because it is
copy-by-default and needs the caller to say `std::move`. cflat, being move-by-default, needs neither
the overload nor the ctor for INSERT - the move-sink-for-owners rule (gap 1) plus a call-site
`.copy()` covers it. The member-wise move hook (gap 3) is still wanted for correctness of the move
itself (nulling interior state), not to resolve a fork.

## C++ reference (Rule of Zero, class with a unique_ptr field)

`struct Holder { unique_ptr<R> r; int k; };` - copy ctor implicitly DELETED (member non-copyable ->
non-copyable class), move ctor implicitly DEFAULTED (movable; member-wise move NULLS `r`), dtor frees
once. `Holder b = a;` is a compile error; `Holder b = move(a);` nulls `a.r` so the moved-from dtor is
a no-op. Pass-by-value `f(a)` is a compile error (copy deleted); `f(move(a))` consumes. cflat should
match: reject plain `enqueue(h)` on an owning value, and make `enqueue(move h)` actually null `h`.

## BLOCKER discovered 2026-07-21 (reverted the queue/stack insert collapse)

Stage 4/5 revealed a compiler enabler the plan did NOT identify: the move-sink consumes NAMED owning
locals and `string` temps, but NOT owning-value RVALUE TEMPORARIES - an owning value-STRUCT temp
(`x.copy()`) double-frees (rc 134) and a fat-CLOSURE element (`container<Lambda>`) aliases + crashes
(rc 138), because the old copy-on-insert deep-cloned them and the collapsed `slot = move value` does
not. This made the shipped queue/stack insert collapses latently unsound (a regression vs the old
containers) and breaks 7 examples under a list collapse. Full write-up + fix direction:
`internal/issue/move-sink-does-not-consume-owning-rvalue-temps.md`.

RESOLUTION: the queue.cb + stack.cb INSERT collapses were REVERTED to the sound `if const`
copy-on-insert form (queue keeps its sound Stage-1 dequeue collapse). The sound compiler enablers
stay: Stage 2 (diagnostic), Stage 3 (named-local move-sink), Stage 4's generic-method sink wiring.

NEW REQUIRED STAGE (call it 3.5, HIGH risk, gates 4/5): extend the move-sink to soundly consume an
owning-value RVALUE TEMP - recognize an owned struct/closure temp (in `pendingOwnedStructTemps` /
`pendingOwnedClosureTemps`) as a consumable owner, UNREGISTER it on consume (add an owned-struct-temp
unregister; make closure elements sink-eligible so the env transfers instead of aliasing). Only AFTER
3.5 (with `container<Lambda>` / `container<owning-struct>.insert(x.copy())` added to the leak matrix)
can queue/stack be re-collapsed and list/dictionary collapsed. Until then the containers stay
`if const` on insert.

## Stages (agent-ready)

Ordered by dependency and risk. Each stage is independently verifiable and leaves ALL suites green
(`test.sh Release`, `example_mac.sh`, `test_lsp.sh`) unless it lists an intended, enumerated test
delta. Do stages in order; 5 fans out. Every stage inherits the cross-cutting constraints at the end.

The pilot is `queue`; `list`/`stack`/`dictionary` follow the proven shape in stage 5. `queue.cb` is
currently the UNSOUND collapsed spike - Stage 0 resets it, so start there.

### Stage 0 - Baseline reset + coverage net (no behavior change)   [sonnet, low risk]
- Do: `git checkout HEAD -- cflat/core/queue.cb` to undo the collapsed spike. Extend
  `Test/test_collection_leaks.cb` with a `queue` leg for EVERY element kind - `int`, a trivial value
  struct `Point{int x,y}`, `string`, a `Holder{unique R* r}`, bare `R*`, `unique R*`, an interface
  value, `unique IShape` - each doing insert / peek / dequeue / scope-exit teardown under `HeapAudit`
  (assert no leak, no double-free/134). This matrix is the regression net every later stage must keep.
- Files: `cflat/core/queue.cb` (revert), `Test/test_collection_leaks.cb` (extend, do not create new).
- Verify: all three suites green with the new matrix passing.
- Depends: none.

### Stage 1 - Sound read-side collapse (library only, NO compiler change)   [sonnet, low risk]
- DONE (partial, 2026-07-21): `dequeue` collapsed to `T v = move _data[_front]; _advance(); return v;`
  (one plain-`T` overload, no `if const`) - SOUND and shipped (suites green). The moved-out value is
  RETURNED, never dropped in-body, so the mis-owning drop path is never hit.
- BLOCKED: the `_releaseAt` -> `T value = move _data[index];` collapse is NOT sound library-only.
  The earlier "validated sound" claim was wrong: at TEARDOWN the move-out-into-a-dropped-local
  path mis-owns for two element kinds - a bare `T*` BORROW over-owns (spurious `delete` ->
  double-free) and a `unique IShape` UNDER-owns (no free -> leak); `unique T*` and value/string are
  fine. `_releaseAt` therefore stays `if const` until the compiler gap is fixed. This is a READ-side
  sibling of the Stage 3 (gap 1) "owns a resource" enabler - do them together. Full write-up:
  `internal/issue/move-out-into-dropped-local-ownership.md`.
- Files: `cflat/core/queue.cb` (dequeue collapsed; `_releaseAt` annotated + kept `if const`).
- Verify: suites green; the Stage-0 matrix passes unchanged (HeapAudit clean for all kinds).
- Depends: 0. Re-collapse `_releaseAt` as part of Stage 3.

### Stage 2 - Compiler SAFETY NET: reject the owning-value double-free (gap 2)   [opus, medium risk]
- DONE (2026-07-21): extended `DiagnoseExplicitMoveToBorrowParam`'s owning predicate with
  `IsOwningValueType(T)`. Root cause of the silent-accept: `TypeHasDestructor` only sees a
  USER-declared `~`, so a value struct that owns via a SYNTHESIZED dtor (`Holder{unique T*}`) was
  invisible to it (a `string` already fired via `IsOwningString`). `IsOwningValueType` forces the
  memberwise dtor and checks it, so `move <owning-struct>` into a plain non-move param now errors
  with the same message as the string case. This is the EXPLICIT-`move` half of the net.
- SCOPED OUT (folded into Stage 3): the "plain by-value alias path" (a NO-`move` `enqueue(owner)`
  that stores-and-escapes). It is not a live miscompile today - the shipping containers COPY a
  copyable owner and POISON a non-copyable one, so nothing silently aliases. The escape-analysis
  guard the plan's Open Q describes only becomes necessary WHEN Stage 4 removes that copy/poison,
  and it is the same "owns a resource" reasoning as Stage 3's move-sink enabler - so it is done
  there, not as a separate escape analysis here.
- Files: `cflat/LLVMBackend.h` (predicate, +3 lines), `Test/errors/err_move_owning_value_to_borrow_param.cb`.
- Verify (met): `move Holder` / `move string` into a plain param both error; test.sh 468/0,
  example_mac.sh 35/0, test_lsp.sh 152/0 - zero false positives from the wider predicate.
- Depends: 0. Open Q (deferred to 3): predicate for the no-`move` "would alias-and-escape" case.

### Stage 3 - Compiler ENABLER: owning-value by-value param becomes a move-sink (gap 1)   [opus, HIGH risk]
- DONE (2026-07-21). A plain by-value `T value` param the callee body UNCONDITIONALLY moves is a
  synthesized move-sink: the caller's owning source is nulled at the call site (move-by-default,
  no keyword), exactly as `IsUniqueTypeArg` already does. Read-only params (`int len(string s)`) are
  untouched and keep borrowing.
- Design decisions on the open questions:
  - Inference precision: "body moves the param" = a top-level `move <bare-param>` NOT nested in any
    if/while/for/switch/labeled/lambda/ternary, AND NOT preceded by a statement that can return early
    (a `return` reachable before the move). The early-exit guard is load-bearing: without it
    `f(T v){ if (b) return; g = move v; }` marks a sink and LEAKS on the b=true path (caller nulled,
    move skipped). A side-effecting `if` with no return (`if (cap) _grow(); _data[..] = move value;`)
    still marks a sink - that is the Stage-4 container shape. When unsure, do NOT mark (fallback to
    the copy/borrow path is always sound).
  - Owns-a-resource: applied at the CONSUMPTION site on the concrete type (`IsOwningValueType(T) ||
    T=="string"`), plus the matched arg must itself be an OWNER (borrow/alias args are never nulled).
    So a non-owning T (int, bare `T*`, interface borrow) never consumes, even if the body says `move`.
  - Generics: the definitive owning check runs per-instantiation at the call site on the monomorphized
    `param.TypeName`, so sink-ness is naturally per-T.
- Files: `cflat/MainListener.h` (ForwardRefScanner body-scan: `CollectUnconditionalMovedNames` +
  `SubtreeContainsFunctionReturn` + `ParamIsOwningSinkEligible`, set `IsOwningSink`), `cflat/LLVMBackend.h`
  (`TypeAndValue::IsOwningSink` field; `ApplyMoveParamTransfer` gate; `DiagnoseExplicitMoveToBorrowParam`
  treats a sink as a move), `cflat/LLVMBackend.cpp` (`--init` round-trip `osk`, verified warm-cache),
  `Test/test_collection_leaks.cb` (direct non-container sink regression, HeapAudit-clean),
  `Test/errors/err_owning_value_sink_consumes.cb` (use-of-moved contract, string + Holder).
- Verify (met): gates a-e all pass; early-return leak repro is leaks=0; test.sh 470/0 (cold AND warm),
  example_mac.sh 35/0, test_lsp.sh 152/0.
- Depends: 2. Note: ScanFunctionDefinition returns early for generic templates, so CONTAINER inserts
  are not yet marked - that wiring is Stage 4.

### Stage 4 - Collapse queue INSERT to move-by-default (uses stage 3)   [opus, medium risk]
- LIBRARY COLLAPSE REVERTED (2026-07-21) - latently unsound for owning-value rvalue temps / closures
  (see the BLOCKER section above). The generic-method sink WIRING below is sound and KEPT; only the
  queue.cb insert change + its test deltas were reverted to `if const` copy-on-insert. Re-do after 3.5.
- DONE (wiring, kept). PREREQUISITE solved first: the Stage-3 sink inference did not reach
  generic-CLASS-method instantiations (proven by a `Box<string>::put` repro compiling clean). Two
  registration paths bypassed it - generic free fns via `ParseFunctionDefinition`, and generic-class
  methods via `PreDeclareInstantiationMembers` (which pre-declares, so `ParseFunctionDefinition` sees
  `alreadyDeclared` and never re-registers). Fix: lifted the three scanner helpers + a new
  `ApplyOwningSinkInference` to free `inline` functions (structural, T-independent) and called it at
  BOTH monomorphized-param registration sites (`ParseFunctionDefinition` + both `PreDeclareInstantiationMembers`
  branches). No LLVMBackend.cpp change - the `osk` round-trip already exists and generic templates are
  re-inferred per compile (never in the core `--init` bitcode; warm-cache verified). The owns-a-resource
  gate at the call site keeps it inert for non-owning T (`Box<int>`/`Box<T*>` do not consume).
- Then collapsed queue insert: single `enqueue(T value){ if(grow) _grow(); _data[slot]=move value; }` -
  deleted `_placeAt`, the `enqueue(move T)` overload, the insert `if const`, and the `!is_copyable`
  poison. `enqueue(owner)` consumes (no keyword), `enqueue(borrow)` aliases, `enqueue(x.copy())` keeps x.
- `_releaseAt` STAYS `if const` (the read-side move-out-into-dropped-local gap,
  `internal/issue/move-out-into-dropped-local-ownership.md`). So queue is NOT fully `if const`-free -
  only the INSERT `if const` dissolved; the teardown one waits on the read-side enabler. The plan's
  "ZERO per-element if const" acceptance is therefore partially met (insert side) pending that enabler.
- Intended test deltas: `queue<string>` leg now uses `enqueue(qsrc.copy())` to keep the source +
  plain `enqueue(qmv)` to show move-by-default; `queue<LeakHolder>` leg drops the redundant `move`;
  `err_queue_noncopyable_elem.cb` premise flipped to "plain enqueue MOVES a non-copyable owner ->
  reuse is use-of-moved". stack/list/dictionary untouched (Stage 5) and still copy (their moves are
  inside `if const`, a `selectionStatement` in the stop-list, so never marked sinks - verified: the
  `stack<string> push copies (source)` assertion still holds).
- Files: `cflat/MainListener.h` (generic wiring), `cflat/core/queue.cb`, `Test/test_collection_leaks.cb`,
  `Test/errors/err_queue_noncopyable_elem.cb`.
- Verify (met): queue matrix HeapAudit-clean (312/312, no 134); `Box<string>::put` consumes,
  `Box<int|T*>::put` do not; queue.cb insert has ZERO `if const`; test.sh 470/0 (cold AND warm),
  example_mac.sh 35/0, test_lsp.sh 152/0.
- Depends: 3.

### Stage 5 - Propagate to list, stack, dictionary   [sonnet x3, parallel, low-medium risk]
- Do: replicate Stage 1 + Stage 4 per container (they share the shape). Read-side collapse stays
  PARTIAL everywhere: the pop/dequeue/take return-kind `if const` collapses (proven), but
  `_releaseAt`/`_releaseValue` STAY `if const` (the move-out-into-dropped-local blocker). Whole-container
  `copy()` keeps its `if const` too (per acceptance). NOTE: not cleanly parallel - the containers share
  `Test/test_collection_leaks.cb`, so the test deltas must be serialized (one editor at a time).
- STACK: REVERTED (2026-07-21). The push/pop collapse was shipped then reverted to sound `if const`
  copy-on-insert - same latent-unsoundness blocker as queue (owning-value rvalue temps / closures,
  see the BLOCKER section). Re-do (push AND pop) after 3.5. stack.cb is back to full `if const`.
- LIST: TODO. Bigger blast radius - two insert methods (`add`, `set`), each with a plain form + poison
  + a `move T` overload, plus `_placeAt`; many call sites across tests/examples; subtle `string`
  adopt semantics. Collapse add/set to single sinks; keep `_releaseAt` + `copy()` `if const`. Also
  collapse `take`'s return-kind (Stage-1 read side) if present.
- DICTIONARY: TODO. Collapse the VALUE store (`add`/`set` value side): remove the value `if const` in
  `_storeValue`, the add/set poison, the `add(move V)`/`set(move V)` overloads. KEEP the KEY `if const`
  (keys are not the owning-value insert), `_releaseValue`/`_freeValue` `if const`, and `copy()`.
- Files: `cflat/core/list.cb`, `stack.cb` (done), `dictionary.cb` (+ their tests / err tests).
- Verify: per-container matrix; `test.sh` + `example_mac.sh` green (examples exercise these heavily).
- Depends: 4.

### Stage 6 - (DEEP, deferrable) model `string` as an owning value (gap 4)   [opus, high risk]
- Give `string` a `unique`-style owning buffer so `IsOwningValueType("string")` is true, and delete
  the `TypeName == "string"` special cases so it flows through ONE owning-value path. Largely
  independent of 4/5; defer until the container shape is proven. Depends: conceptually 3.

### Stage 7 - (DEEP, deferrable) member-wise move / move-ctor hook (gap 3)   [opus, high risk]
- For move CORRECTNESS of types with interior self-pointers (not to resolve any fork - the fork is
  already dissolved). Shared with the pinned-types side-bar in [[unique-ownership]]. Defer.

### Cross-cutting constraints (every stage)
- Any change to type parsing goes in BOTH `ParseDeclarationSpecifiers` copies (ForwardRefScanner +
  MainListener). Errors via `LogError`/`LogErrorContext` only. ASCII only. Inline comments <= 2 lines.
- Any new field on `TypeAndValue`/`StructData`/`AnnotationValue` that an analysis reads MUST be added
  to the `LLVMBackend.cpp` `--init` cache round-trip in the SAME change, or warm-cache tests silently
  stop firing.
- Do NOT create new test files except `Test/errors/err_*.cb`; extend existing tests otherwise.
- Verify on the current host (`test.sh Release`) before declaring a stage done; never dilute an
  assertion to pass; never let a partial change convert a compile error into a silent double-free.
- Give each implementing agent this file plus the specific stage; the main session reviews the diff
  and re-runs the suite before starting the next stage.

Blast radius to respect: stage 3 changes parameter passing for EVERY `f(OwningValue)`, not just
containers - which is why stage 2 (the safety net) precedes it and stage 3 is scoped to params the
body actually moves.

## OUTCOME (2026-07-21): refined design shipped; queue/stack/list DONE, dictionary DEFERRED

### Design REVISION (maintainer decision) - the "zero if const" acceptance is superseded
The move-by-default-for-EVERYTHING collapse was rejected as too aggressive: it consumed COPYABLE
owning values (string, closure, copyable structs), which is not wanted. The chosen model treats a
COPYABLE value - explicitly INCLUDING `string` and `Lambda` closures - as a value type that COPIES on
insert (caller's source stays intact), and MOVES only a NON-COPYABLE owning value (a struct that owns
a `unique` and has no copy(), or a `unique T*`). So insert KEEPS one `if const (is_copyable(T))`
split; the only behavior change from the pre-collapse containers is the non-copyable arm: `compile_error`
poison ("write move") becomes move-by-default. The earlier acceptance line "ZERO per-element
is_copyable dispatch" no longer holds - `is_copyable(T)` is the intended dispatch.

### Insert shape (queue/stack/list - implemented and green)
```
if const (is_copyable(T)) { void add(T value) { ...; _placeAt(slot, value); ... } }  // copy / alias
else                      { void add(T value) { ...; _data[slot] = move value; ... } } // move sink
```
The non-copyable arm's body must contain a TOP-LEVEL `move value` for the by-value param to be
inferred a move sink - an inlined `_data[slot] = move value` OR a call argument like
`_placeAt(slot, move value)` / `helper(..., move value)` both count (a top-level MoveExpression inside
a call arg is matched). A plain `_placeAt(slot, value)` with NO `move` does NOT mark the param a sink
(that was the real cause of the multi-instantiation double-free during Stage 5 - misdiagnosed at the
time as "must not route through _placeAt"; the fix is only that the forward must carry `move`). The
explicit `add(move T)` overload is KEPT (opt into move for a copyable T). `_releaseAt`/`copy()` stay
`if const` (read-side blocker).

### Increment 1 (compiler enabler) - DONE
Closures made owning-VALUE types like string (flow through the same move-sink / use-after-move / temp
cleanup): `ParamIsOwningSinkEligible` admits fat closures, `ApplyMoveParamTransfer` treats a closure
temp/local as a consumable owner, closure INVOCATION checks use-after-move, and a dequeue-return
closure-env leak was fixed. See the closure issue file (updated) for detail.

### Increment 2 (library collapse) - queue/stack/list/dictionary ALL DONE
- queue.cb / stack.cb / list.cb: is_copyable split shipped. Four poison err tests repurposed to
  assert the new use-of-moved contract (`err_{queue,stack,list2,dict}_noncopyable_*`).
- dictionary.cb: DONE (no compiler change needed - the earlier "needs all-paths-consume" conclusion
  was wrong). `add()`'s two consume paths (place-on-insert / free-on-duplicate) funnel through an
  explicit `move V` sink `_placeOrInsertSink`, so the plain `add()` param is inferred a sink (a
  top-level `move` inside a call arg counts). `set()` has no refusal path (single trailing top-level
  move -> sink). Subtlety handled: `unique V*` is is_copyable==TRUE (takes the copyable arm) yet its
  param is a move sink, so a refused duplicate must `_freeValue` it or it leaks (a bare
  `move unique V*` param is not auto-freed at scope exit the way an owning struct is).
- Verified: `test.sh` 472/0 (cold+warm), `example_mac.sh` 35/0, `test_lsp.sh` pass.

## Acceptance (revised)

`queue|stack|list<int|Point|string|Lambda|Holder|CopyableStruct|T*|unique T*|IShape|unique IShape>`:
insert/read/iterate correct, teardown frees each owned resource exactly once (HeapAudit clean, no 134),
borrows never freed, `add(copyable)` COPIES (source intact), `add(non-copyable-owner)` MOVES by default
(source consumed, no keyword), `add(borrow)` aliases. Insert carries exactly ONE `is_copyable(T)`
split (by design); `_releaseAt`/`copy()` retain their `if const`. dictionary reaches the same shape
once the all-paths-consume sink lands.
