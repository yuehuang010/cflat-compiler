# Ownership-transparent assignment: `dest = src` defers to T

Opened 2026-07-22. COMPLETE 2026-07-23 (R4 close-out; see Status). Two follow-ons live in
internal/issue/: move-copyable-param-overload-uaf.md (pre-existing UAF, gap 2) and
discard-move-global-string-leaks.md; btree _freeValue retirement needs borrowed-slot
ownership recovery (noted in STEP R3 DONE). GOAL, multi-change, iterative. This is "Option 5" from the container
brainstorm - the maximal successor to [[container-ownership-transparency]]: instead of
containers dispatching ownership with `if const`, ASSIGNMENT ITSELF becomes total over T:

    copy  if T is a copyable owner (string, list, closure, struct with real/synthesized copy())
    move  if T is a non-copyable owner (unique T*, unique <iface>, struct owning a unique)
    alias if T is a borrow (bare T*, alias T*, interface value)
    copy  if T is trivial (primitive, POD struct)
    ...and the OLD destination value is always dropped first (exactly one owner at all times).

Once slot stores share these semantics, container `_placeAt`/`_storeValue` ladders collapse
to `_data[i] = value;` with zero `if const`. Related: [[unique-ownership]],
[[ownership-move-alias-discipline]], `internal/issue/move-out-into-dropped-local-ownership.md`.

## Feasibility finding (2026-07-22 probe): 7 of 10 categories ALREADY match the target

Empirical matrix (59 probes, HeapAudit oracle; probes were under gitignored
`scratch/assign_probes/`, key repros preserved in the issue files below). Contexts:
A init from named local, B reassign over live value, C self-assign, D heap array-slot store,
E struct-field store.

| element T                     | today (A/B)         | target | delta                        |
|-------------------------------|---------------------|--------|------------------------------|
| primitives, POD struct        | COPY                | copy   | none                         |
| bare `R*`, interface value    | ALIAS               | alias  | none                         |
| `Holder{unique R*}` (no copy) | MOVE (ct-enforced)  | move   | none                         |
| `Lambda` closure              | CLONE + drop-old    | copy   | none - already the model     |
| `string`, `list<int>`, `Row`, `CRow` | MOVE (ct-enforced) | copy | THE semantic flip (Part 4) |
| `unique R*`                   | move, 3 holes       | move   | bugfixes (Parts 2-3)         |
| `unique IThing`               | over-strict         | move   | bugfix (Part 3)              |

The closure path (clone-by-default, clone-source-before-freeing-old-dest, self-assign safe)
is the reference implementation of the target semantics, already shipped for one category.

Soundness holes found by the probes (real shipping bugs, filed as issues):
- `internal/issue/unique-local-reassign-leaks-old-pointee.md` - `b = a;` leaks b's old pointee.
- `internal/issue/unique-self-assign-crash.md` - `a = a;` segfaults (self-move).
- `internal/issue/unique-field-store-unguarded-alias.md` - `w.f = a` silent alias, UAF one
  perturbation away (X4 guard is aggregate-only).
- `internal/issue/unique-interface-move-rejected.md` - `b = move a` impossible for unique
  interfaces (predicate divergence vs thin unique).

Also observed (context D/E gaps, separate from ownership): grammar has no `new T*[n]` heap
array of pointer elements; `new Lambda<...>[n]` is "unknown type"; interface pointers are
rejected outright. These block containers-of-those-types from using raw `new T[]` buffers
but do not block this plan (containers allocate through their own machinery).

## Code survey: where assignment lives (2026-07-22)

- `ParseAssignmentExpression` MainListener.h 9107-9983 (~876 lines, ~40 inline cases, one
  function). Destination kinds discriminated inline: local/global alloca, struct-field
  2-index GEP (X4 guard at 9580), interface byte-offset field, deref `*p` (9820-9855),
  bitfield/union; SINGLE-INDEX GEP (container slot) deliberately EXCLUDED from all
  move/drop logic (comments 9581, 9862, 9906).
- Decl-init is a SEPARATE hand-mirrored path: `ParseDeclaration` 7317-~8504 (move/copy logic
  8186-8496; comments literally say "mirrors the declaration initializer path").
- Brace-init field stores: `EmitOneFieldInit` 12405; compound ops inline at 9556-9578;
  `x[k] = v` desugars to `.set()/.add()` (13164) and user `operator[]` (~15892).
- Drop-old-destination is SCATTERED over five type-specific blocks: unique-iface local
  9528-9539 (DeleteInterfaceValue), closure 9595-9625 (clone + FullDestructor), owning
  struct field/local 9857-9878, unique FIELD 9880-9899 (EmitUniqueFieldDelete), string
  local 9901-9916. The scatter is where the holes live.
- Reusable ingredients that ALREADY exist:
  - per-type release ladder: `EmitDestructorsForScope` LLVMBackend.h 2008-2113 (+
    EmitOwningPtrCleanup 1720, EmitOwningInterfaceCleanup 1785, EmitOwningUniqueArrayCleanup
    1807) - the classifier a total drop reuses;
  - `IsOwningValueType` 3420, `is_copyable` MainListener.h 17157-17189,
    `TypeOwnsUniquePointer` 3435;
  - synthesized memberwise copy: `GetOrCreateMemberwiseCopy` LLVMBackend.h 3540-3615,
    triggered from CreateOverloadedFunctionCall 14465-14498;
  - move-sink machinery: ApplyMoveParamTransfer LLVMBackend.h 10195-10383,
    ParamIsOwningSinkEligible MainListener.h 927-942.

## Parts (each independently shippable; ordered by dependency and risk)

### Part 1 - Extract `DropValue(var, storage)` (refactor, NO behavior change)   [sonnet, low risk]
- Factor the per-type release ladder out of `EmitDestructorsForScope` into one reusable
  helper: delete for thin `unique T*`, vtable-dtor+free for unique interface, full dtor for
  owning value/string/closure, no-op for borrows/primitives. Scope-exit calls it per local.
- BONUS: this helper backs the user-facing EXPLICIT RELEASE form `_ = move <local>;`
  ("move to nowhere") - decided 2026-07-23 as the one spelling.
- Files: `cflat/LLVMBackend.h`. Verify: all suites green, zero test deltas.
- Depends: none.

### Part 2 - Total drop-old-destination on reassign   [opus, medium risk]
- Route the five scattered destruct-old blocks through DropValue and COVER THE HOLES:
  unique-LOCAL reassign frees the old pointee (fixes the leak issue), unique FIELD store
  stops silently aliasing (fixes the unguarded-alias issue).
- Ordering rule everywhere: produce/clone the source value BEFORE dropping the old dest
  (the closure block is the pattern); keep the `destination != source` self-assign guard.
- Files: `cflat/MainListener.h` (ParseAssignmentExpression), `cflat/LLVMBackend.h`.
  Extend `Test/test_collection_leaks.cb` reassign legs; new `Test/errors/err_*` as needed.
- Verify: both issue repros green under HeapAudit; suites green.
- Depends: 1. Deletes: unique-local-reassign-leaks-old-pointee.md,
  unique-field-store-unguarded-alias.md (delete the issue files when fixed).

### Part 3 - Unique edge unification   [opus, low-medium risk]
- Fix the self-assign segfault (landed as a guard; ff55e6b).
- Unify the two ownership predicates so `move <named unique local>` transfers for unique
  INTERFACES exactly as for thin uniques (init AND reassign), consuming + nulling the
  source, dropping any old dest via DropValue.
- Files: `cflat/MainListener.h` (8487-8496, 9291-9293, 9517-9539). Err tests updated.
- Verify: issue repros; suites green.
- Depends: 2 (uses DropValue on the old interface value). Deletes:
  unique-self-assign-crash.md, unique-interface-move-rejected.md.

### Part 4 - THE FLIP: copyable owners COPY on assign   [opus, HIGH ecosystem risk - DECISION GATE]
- Where T is a copyable owner (string, containers, structs with real or synthesized copy();
  closures already do this), `dest = src` from a NAMED source invokes `.copy()` instead of
  moving; non-copyable owners keep move-with-consume; rvalue/call-result sources keep
  transferring (no copy of a temp). Explicit `move` stays the opt-out fast path.
- Why this is the decision gate:
  - It REVERSES the documented move-on-assign design for string/containers. But it is the
    same policy the maintainer already chose for container INSERT in
    [[container-ownership-transparency]] ("copyable copies, non-copyable moves") - this
    part makes `=` agree with that policy.
  - Correctness-compatible: today's `b = a; use(a)` is a compile error, so no compiling
    program changes observable behavior; previously-rejected programs start compiling
    (including the X4 field-store errors, which become working copy semantics).
  - The cost is SILENT PERF: deep copies where moves used to happen. Mitigation: keep
    explicit `move`; a later last-use analysis (source dead after assign -> move anyway)
    recovers most of it - deferrable, do not block on it.
  - Test deltas are real: every test/err-test asserting use-of-moved after `b = a` flips.
    Enumerate them FIRST (grep for the assertion pattern) and list the delta in the stage
    report before flipping.
- Sub-stages (each verifiable): 4a locals reassign, 4b decl-init, 4c field store (replaces
  X4 + string-field errors), 4d deref stores. Compound ops keep read-modify-write.
- Files: `cflat/MainListener.h` (both paths - note the hand-mirroring; Part 5 removes it),
  `cflat/LLVMBackend.h`. Broad test deltas.
- Verify: suites green with the enumerated delta only; probe matrix cells for
  string/list/Row/CRow become COPY-ok in A/B/C/E.
- Depends: 2. GATE: maintainer sign-off on the flip before implementation.

### Part 5 - Unify the assignment paths   [opus, large but mechanical, incremental]
- Route decl-init (`ParseDeclaration` 8186-8496), brace-init (`EmitOneFieldInit`), reassign,
  and the deref path through ONE shared classify-and-store helper so the hand-mirroring
  ends. Land file-by-file / path-by-path; every step suite-green.
- Files: `cflat/MainListener.h`. Verify: suites green, zero behavior deltas after Part 4.
- Depends: 4 (semantics settled first, then consolidate).

### Part 6 - Container payoff: slot stores join the semantics, ladders collapse   [opus, medium risk]
- Lift the deliberate single-index-GEP exclusion so `_data[i] = value` in generic code gets
  the total semantics; then collapse `_placeAt`/`_storeValue` in queue/stack/list/dictionary
  to a plain store, and `_releaseAt`/`_freeValue` to a move-out release (landed as
  `T tmp = move _data[i];` - see STEP D in Status).
- Two things to prove here:
  - drop-old on a DEAD slot is a no-op (containers zero moved-out slots; the leak matrix
    must show teardown/insert-over-dead-slot clean for every element kind);
  - sink inference interplay: the method-level `is_copyable(T)` split stays as the one
    surviving `if const` (it controls sink-ness and is the intended policy split anyway).
- Files: `cflat/MainListener.h`, `cflat/LLVMBackend.h`, `cflat/core/{queue,stack,list,dictionary}.cb`,
  `Test/test_collection_leaks.cb`.
- Verify: full leak matrix green; container `if const` count drops to the policy split only.
- Depends: 4, 5. The read-side move-out-into-dropped-local bug was fixed directly as the
  STEP D enabler.

### Part 8 - Collapse the enqueue/add SINK split (conditional-consume sink params)   [opus, medium-high risk - DECISION GATE]

The last container `if const (is_copyable(T))` splits (queue.enqueue, stack.push, list.add/set,
dictionary.add/set + its `_placeOrInsertSink`/`_freeValue` machinery and the inner
`is_unique(V)` duplicate-free) exist ONLY for SINK-ness: whether the caller's argument is
consumed. Store semantics are already total (Part 6); sink inference is not - it requires a
literal, UNCONDITIONAL top-level `move value` in the compiled arm (ApplyOwningSinkInference /
CollectUnconditionalMovedNames, MainListener.h ~880-960), so the two arms differ only in
`_placeAt(slot, value)` vs `_data[slot] = move value`.

- PROBED 2026-07-23 (scratch/sink_probe.cb): a plain by-value param of a non-copyable owning
  struct COMPILES TODAY AS A BORROW (read-only body: caller keeps the value, no param dtor).
  Therefore a blanket type-based "non-copyable owner by-value = sink" rule would change the
  behavior of EXISTING VALID programs - rejected. The rule must be consumption-gated.
- THE RULE (proposed): a plain by-value param whose monomorphized type is a NON-COPYABLE OWNER
  (owns-resource && !IsCopyableType; ParamIsOwningSinkEligible shape) and whose body consumes it
  on AT LEAST ONE path (source of a plain `=`/slot store that classifies as MOVE for this
  instantiation, or an owning call-arg - no `move` keyword required) becomes an OWNING SINK:
  the caller's named arg is consumed at the call site (as today's inferred sinks), the param
  slot OWNS the value, and any path that does not move it out DROPS it at scope exit via
  DropValue (moved-out paths see a nulled slot - no-op, same as locals). Read-only bodies stay
  borrows: zero delta for existing code.
- Why conditional consumption becomes sound: today's inference demands UNCONDITIONAL move
  because consuming the caller on a not-moved path leaks. Total scope-exit DropValue on the
  sink param closes exactly that leak, so a conditional store (dictionary add's duplicate
  refusal) no longer needs the `_placeOrInsertSink` funnel or `_freeValue` - the refused value
  drops at scope exit.
- Sub-stages:
  - 8a: total scope-exit release for sink params (inferred AND explicit `move T`) via DropValue.
    This also fixes the murky bare-`unique V*` move-param behavior (dictionary.cb's two comments
    at ~154 and ~220 contradict each other on whether it auto-deletes; make it DropValue-total
    and retire the empty-body `_freeValue` pattern - also used by btree.cb, update it too).
    Enumerate double-free risk: existing `move T` bodies that hand-free AND rely on no exit drop.
  - 8b: extend inference per THE RULE (classification-based consume detection + the
    at-least-one-path relaxation, gated to non-copyable-owner param types only).
  - 8c: collapse the containers: single-body enqueue/push/add/set with `_data[slot] = value;`,
    dictionary add becomes `if (insertSlot == -1) return false;` + plain stores; delete
    `_placeOrInsertSink`, `_freeValue`, the `is_unique(V)` inner free. Keep the `move T`
    transfer overloads (`!is_pointer` guards) - they remain the copyable opt-out.
- Deltas to enumerate BEFORE flipping (Part 4 discipline): user functions whose body consumes a
  non-copyable-owner by-value param on some path (previously a leak or an error - flag every
  test/err-test that flips); overload identity (IsOwningSink must not change signature matching
  - verify `add(move T)` still coexists/resolves against a now-sink plain `add(T)`);
  `--init` round-trip for any new flag (IsOwningSink already ships as "osk"); grow-loop
  `newData[i] = move _data[j]` stays untouched.
- LOAD-BEARING INVARIANT (probed 2026-07-23, both HeapAudit-clean): once elements are INSIDE a
  container, all internal motion is move-semantics with zero copies - but two paths spell it as
  RAW BIT-COPY relocation that works only because slot-to-slot GEP stores and non-owning named
  sources bypass the Part 6 owning-store gate: dictionary REHASH (`_keys[slot] = oldKeys[i]`
  bit-copy + `delete[_]` buffer-only free) and list SORT/_partition (`T tmp = _data[i]` borrow
  bit-shuffle swaps; probed with string and owning-struct elements - suites only sort int/Res*).
  Any future widening of the slot-store gate to GEP/element sources would double-copy or
  double-free BOTH paths - re-probe them on any change there.
- Verify: new leak legs - dictionary add-duplicate refusal across every V kind; caller-consumed
  (err use-after-move) + caller-keeps (copyable) legs for enqueue/add/set; suites cold+warm;
  remaining container `if const` = only the `!is_pointer` transfer-overload guards and the
  copy() diagnostic ladders.
- Depends: 6. GATE: maintainer sign-off on THE RULE (it makes previously-leaking/rejected
  conditional-consume bodies well-defined and changes explicit `move T` param exit semantics).
- SPIKE 2026-07-23 (8c-first, .cb-only, restored after): collapsed enqueue/push/add/set to the
  single `_placeAt(slot, value)` body with NO compiler change and probed every matrix row.
  RESULT: every row works TODAY except one - the non-copyable owning VALUE struct, which
  COMPILES, does NOT poison the caller, and DOUBLE-FREES (the Part 6 slot store classifies as
  MOVE and steals the param's unique ptr, but the param entered as a BORROW - store defers to
  T, ENTRY does not; that asymmetry is the entire gap). `unique R*`/`unique IThing` rows work
  because unique params already auto-sink. Blast radius under collapse: exactly the 3 err tests
  asserting "use of moved variable 'h'" (x cold+warm = 6 fails); leak oracle stayed 405 green
  (the positive suite never plain-inserts a non-copyable value - the whole signal lives in
  those err tests; add positive consumed-caller legs when 8b lands). Overload-identity item
  answered: the single plain add(T) coexists and resolves correctly against add(move T).
  MINIMAL DELTA CONFIRMED: 8b (sink inference accepts a MOVE-CLASSIFIED plain store, gated to
  ParamIsOwningSinkEligible types) is the functional fix; 8a (scope-exit DropValue on sink
  params) is the soundness invariant behind it - runtime no-op for these four methods,
  mandatory before dictionary's conditional-consume add. They ship together per THE RULE.

#### API-surface matrix: `list.add(x)` semantics per monomorphized T (probed 2026-07-23)

The call site is invariant (`l.add(x)`); T's monomorphization ALONE decides copy-vs-move-vs-alias.
Empirical (scratch probes, HeapAudit + dtor counters, all green). Same rules apply to
queue.enqueue / stack.push / list.set / dictionary add/set VALUES (dictionary KEYS always copy -
separate policy).

| element T                             | class            | caller's x after add | slot gets      | frees        |
|---------------------------------------|------------------|----------------------|----------------|--------------|
| primitive / enum / POD struct         | trivial          | kept                 | bit copy       | n/a          |
| `string`                              | copyable owner   | kept, independent    | deep copy      | both, once each |
| struct w/ hand-written `copy()`       | copyable owner   | kept (2 indep frees) | `copy()`       | both, once each |
| struct w/ synthesized memberwise copy | copyable owner   | kept                 | synth copy     | both, once each |
| container (`list<int>` etc. as T)     | copyable owner   | kept                 | `copy()`       | both, once each |
| `Lambda<...>` closure                 | copyable owner   | kept                 | env clone      | both, once each |
| struct owning `unique`, no `copy()`   | NON-copyable     | CONSUMED, name poisoned | moved       | container    |
| struct w/ user dtor + raw ptr, no `copy()` | NON-copyable (SynthCopyUnsafe) | CONSUMED, poisoned | moved | container |
| bare `R*` / `alias R*`                | borrow           | kept, pointee untouched | aliased     | real owner   |
| interface value (`IThing`)            | borrow (fat view)| kept                 | aliased        | real owner   |
| `unique R*`                           | owning ptr (auto-sink) | CONSUMED, poisoned | transferred | container (`delete`) |
| `unique IThing`                       | owning iface     | CONSUMED, poisoned   | transferred    | container (vtable dtor) |

- POISONING RULE (stronger than expected): consumption at a CALL-ARG position marks the caller's
  name moved and ANY later read is a compile error ("use of moved variable") - including reads
  that just null-check. This applies to implicit sink consumption (`l.add(holder)`) AND the
  explicit spelling (`l.add(move s)`) alike. Contrast: local-to-local `b = move a` leaves `a`
  readable-as-null by design. So the call-arg form is already stricter than assignment.
- Caller opt-outs are the ONLY degrees of freedom: `add(move x)` forces transfer of a copyable
  owner (overload exists for value types only - `!is_pointer(T)` guard); `.copy()` at the call
  site pre-copies. There is no opt-in to copy a non-copyable (by construction).
- Part 8 relevance: the matrix is already total and caller-visible-stable; Part 8 changes NO
  cell - it only removes the `if const` scaffolding that implements the copyable/non-copyable
  row split, and makes dictionary add's duplicate-refusal path uniform.
- Syntactic wrinkle (found adding the sort legs): a lambda PARAMETER list cannot carry the
  `unique` qualifier - the compare for a `list<unique PtrLeak*>` is written with the bare
  pointee type, `(PtrLeak* a, PtrLeak* b) => ...`, and substitution still matches
  `Lambda<bool(T,T)>` (the element is handed to the compare as a borrow).
- Coverage (2026-07-23): sort-on-owning-elements and forced-rehash legs added to
  test_collection_leaks.cb (internal count 374 -> 393); suites green.

### Part 9 - Collapse the RESIDUAL container `if const` (full inventory, 2026-07-23)

Everything left in queue/stack/list/dictionary after Parts 6+8, clustered by what unblocks it.
Target end state: ZERO ownership-driven `if const` in the containers; the only residue is
explicitly-accepted policy (listed at the end).

#### 9a - Release/return unification   [sonnet, .cb-only, NO compiler change - available NOW]
- Dictionary remove()/clear()/~() each inline the SAME V-release ladder `_releaseValueAt`
  already collapsed (dict 395-402, 429-435, 504-513: `is_unique(V)` delete / `!is_pointer(V)`
  destruct / key `!is_primitive(K) !is_pointer(K)` destruct - 12 `if const`). Route all three
  through `_releaseValueAt(slot)` plus a new `_releaseKeyAt(slot)` = `_ = move _keys[slot];`
  (the discard slot-move handles owning string/struct keys, is a no-op for primitives, frees
  nothing for pointer borrows - same DropValue recovery as values).
- list ~() keeps a `is_unique(T)` free-loop (list 341) and relies on `delete[_size]` running
  value dtors; collapse to the clear()-style `_releaseAt(i)` loop + buffer-only `delete[_]`
  (uniform with queue/stack teardown, which already loop `_releaseAt`).
- stack.pop() still carries the 3-way return-kind ladder (stack 112-137: `move T` / `alias T`
  / `move T`). queue.dequeue() and list.take() ALREADY ship the collapsed form - plain
  `T pop() { _size--; T value = move _data[_size]; return value; }` defers to T (a
  `stack<alias T*>` still rejects deleting the result). Mirror it.
- Verify: leak matrix (393) + new legs for pop-on-owning-elements and dict remove()/clear()
  over every K/V kind; suites cold+warm.
- DONE 2026-07-23 (dictionary + stack legs); list ~() DEFERRED. Dictionary remove()/clear()/~()
  now route V+K release through `_releaseValueAt(slot)` + a new `_releaseKeyAt(slot)` =
  `_ = move _keys[slot];` (all 12 `if const` gone); stack.pop() collapsed to the plain
  `T pop() { _size--; T value = move _data[_size]; return value; }`. Both .cb-only, no compiler
  change. The list ~() collapse could NOT land: swapping the direct `is_unique(T)` delete loop for
  the `_releaseAt` discard-release LEAKS a self-referential owning element (a `list<unique TreeBox*>`
  monomorphized while TreeBox is incomplete - test_move.cb::recursive_container_frees_all went 3->0).
  Root cause: the direct `delete` SITE gets deferred/late destructor resolution; the discard-release
  path binds the element dtor early to a null dtor. Kept ~list() as the direct-delete loop; filed
  internal/issue/list-dtor-releaseat-collapse-incomplete-type.md (needs the compiler-side late
  resolution before the collapse - out of 9a's ".cb-only, no compiler change" scope). Existing 393
  leak legs already cover stack.pop / dict remove/clear over every owning K/V kind. Verified:
  leak oracle 393/EXIT=0, test.sh Release 476/0/8, example_mac 35/0, test_lsp 152/0.
- ESCALATION (2026-07-23, probed at HEAD): the deferred-leg root cause was a LIVE LEAK, not just a
  collapse blocker - SELF-REFERENTIAL unique elements (struct holding `<container><unique Self*>`)
  leaked the whole subtree through ~dictionary()/~stack()/~queue() and list clear()/removeAt() (0 of
  3 freed, probe scratch/selfref_leak_repro.cb). Only ~list() survived (kept the direct-delete
  loop) and only list had a self-ref test, which is why suites stayed green.
- RESOLVED + 9a COMPLETE (2026-07-23): one-line compiler fix - EmitOwningPtrCleanup (the thin
  `unique T*` DropValue arm, LLVMBackend.h ~1776) now resolves the pointee dtor through
  GetFullDestructorForDelete instead of the eager GetOrCreateFullDestructor, so a pointee still
  incomplete at emit time binds the same deferred `.dtordeferred` stub the `delete` site uses
  (body patched at finalization) instead of silently dropping the call. With that, the third 9a
  leg landed: ~list() collapsed to the `_releaseAt(i)` loop + buffer-only `delete[_]`, uniform
  with queue/stack. Regression legs added to test_move.cb::testSelfRefContainerRelease (6 legs:
  dict/stack/queue self-ref teardown, dict remove(), list clear()/removeAt(); 137->143).
  internal/issue/list-dtor-releaseat-collapse-incomplete-type.md deleted. Verified: selfref probe
  clean under HeapAudit, test_move 143/143, leak oracle 393/EXIT=0, test.sh Release 476/0/8.

#### 9b - TOTAL `.copy()` over T   [opus, compiler change - the one new totality this part needs]
- Make `.copy()` well-formed for EVERY T: identity bit-copy for primitive/enum/pointer/
  interface-borrow; the real or synthesized copy for copyable owners; the existing actionable
  compile error ("give the type a 'copy()' method, or 'move' ...") for non-copyable owners and
  `unique` pointees. This is the copy-side twin of Part 1's total DropValue.
- Collapses every per-element copy ladder to a one-liner:
  - list.copy() x2 (list 245-248, 272-275) -> `result.add(_data[i].copy());`
  - dictionary.copy() value+key ladders (dict 480-486) -> `result._keys[i] = _keys[i].copy();`
    `result._values[i] = _values[i].copy();`
  - `_placeKeyAt` (dict 123-125) -> `_keys[index] = key.copy();` (the alias-source key store
    MUST copy - a plain `=` from an `alias K` param would alias, which is why this ladder
    could not ride Part 4's flip).
- The top-of-copy() `is_unique` guards (list 235/259, dict 464) fold into the same diagnostic;
  KEEP at most one tailored `if const (!is_copyable(T)) compile_error(...)` per copy() if the
  container-specific message ("...or 'move' the list instead of copying it") is worth more than
  the generic one - decide on message quality at implementation.
- `--init`: any new TypeAndValue/StructData field must ride the cache round-trip same-change.
- Verify: copy() legs across all element kinds; err tests keep firing for non-copyable copy.
- DONE 2026-07-23. The backend's CreateOverloadedFunctionCall already carried a total
  bitwise-identity copy arm (primitives/enums/interface-borrows); the only gap was routing a
  BARE-POINTER receiver of `.copy()` to it instead of auto-deref-copying the pointee. Two edits
  in ParsePostfixExpression (MainListener.h ~15550 arm + stash, ~16744 [PFX-copy-ptr] dispatch):
  a non-owning pointer receiver followed by `.copy` stashes the loaded pointer value/type (the
  auto-deref mutates namedVar before the call is reached) and the call routes it through the
  identity arm; a `unique`/owning pointer never arms the flag, keeping its actionable error.
  NO new serialized fields (function-locals only), warm cache probed explicitly. Ladders
  collapsed: list.copy() x2 -> `result.add(_data[i].copy());`, dictionary.copy() key+value ->
  `.copy()` one-liners, _placeKeyAt -> `_keys[index] = key.copy();`. Instantiation subtlety:
  compile_error poisons but does NOT stop codegen, and unique-element containers eagerly
  instantiate copy(), so the table/loop walks moved into the `else` of the is_unique guards and
  the non-copyable tailored errors sit in dead `if const` branches (kept per plan - better than
  the generic memberwise diagnostic). Tests: +12 copy() legs in test_collection_leaks.cb
  (393->405: int identity, string deep, borrowed-pointer shallow, dict<string,string> deep,
  primitive-local `a.copy()`); new err_list_noncopyable_owner_copy.cb +
  err_dict_noncopyable_owner_copy.cb; err_unique_memberwise_copy.cb retargeted to a Holder
  VALUE (its `h.copy()` on Holder* is now legitimately pointer-identity - same assertion).
  Verified: test.sh Release 480/0/8 (cold+warm), leak oracle 405/EXIT=0 under HeapAudit,
  example_mac 35/0, test_lsp all passed.

#### 9c - `move T` overload elision   [optional; recommend KEEP the guards]
- The 5 `!is_pointer` guards (queue 111, stack 99, list 119/155, dict 305) gate the explicit
  transfer overloads: meaningless for borrows (would strand the element), redundant/ambiguous
  for `unique` (plain param is already a sink). Collapsing needs a compiler rule that SKIPS
  instantiating a `move T` overload when T substitutes to a pointer/interface kind - implicit
  overload elision. RECOMMENDATION: not worth it - the guard is one self-documenting line
  encoding real policy, and silent elision is more magical than the thing it removes. Revisit
  only if overload elision falls out of other work.
#### 9d - Hash dispatch   [optional, orthogonal to ownership]
- `_slot` (dict 41-49) dispatches hashing: `Hash(key)` primitives / `Hash((i64)key)` pointers /
  `key.hash()` structs. Collapse requires a TOTAL `Hash(T)` (identity-address for pointers,
  member `.hash()` required for structs) - a trait-shaped feature, not ownership. Defer unless
  a hashset/btree unification wants it too.
- The `is_unique(K)` and `is_interface(K)` key REJECTIONS (dict 36-44) are policy diagnostics
  with tailored messages - they stay as `compile_error` guards regardless.

#### Accepted permanent residue (after 9a+9b land, if 9c/9d are declined)
- 5x `!is_pointer` transfer-overload guards (policy: who may write `move x` at the call site).
- Up to 3x tailored non-copyable copy() messages (message quality over genericity).
- Dictionary `_slot` hash dispatch + 2 key-policy rejections.
Every OWNERSHIP decision - store, release, return, copy - defers to T with zero `if const`.
- Depends: 9a on nothing (start immediately); 9b on nothing besides Part 4 (landed); 9c/9d
  independent. Part 8 (sink split) remains the separate caller-contract track.

### Cross-cutting constraints (every part)
- Any change to type parsing goes in BOTH `ParseDeclarationSpecifiers` copies. Errors via
  `LogError`/`LogErrorContext` only. ASCII only. Inline comments <= 2 lines.
- Any new field on `TypeAndValue`/`StructData`/`AnnotationValue` that an analysis reads MUST
  be added to the `LLVMBackend.cpp` `--init` cache round-trip in the SAME change.
- Do NOT create new test files except `Test/errors/err_*.cb`; extend existing tests.
- Verify on the current host (`test.sh Release`, `example_mac.sh`, `test_lsp.sh`) before
  declaring a part done; never dilute an assertion; never convert a compile error into a
  silent double-free.

### Roadmap to completion (as of 2026-07-23, after 9a+9b landed and the 8c-first spike)

Remaining work is the Part 8 track plus close-out. Order and tiering:

1. STEP R1 - 8a+8b in ONE compiler change [opus]. 8b: sink inference accepts a MOVE-CLASSIFIED
   plain store/call-arg (no literal `move` keyword), gated to ParamIsOwningSinkEligible types;
   8a: total scope-exit DropValue on owning-sink params (inferred AND explicit `move T`) - the
   invariant that makes 8b's at-least-one-path relaxation sound. Ship together per THE RULE.
   Before flipping, enumerate: existing `move T` bodies that hand-free AND rely on no exit drop
   (double-free risk); the 3 err tests asserting "use of moved variable" must keep firing (via
   inference instead of the literal move); IsOwningSink already serializes as "osk" - any NEW
   flag rides the --init round-trip same-change. Verify: suites cold+warm + POSITIVE
   consumed-caller legs (the spike proved the oracle is blind to this row - all signal
   currently lives in 3 err tests).
2. STEP R2 - 8c simple four [sonnet, .cb-only]. Collapse queue.enqueue / stack.push / list.add
   / list.set to the spike-validated single `_placeAt(slot, value)` body; keep the
   `!is_pointer(T)` transfer overloads. Add leak-oracle legs: consumed-caller (non-copyable)
   and caller-keeps (copyable) across all four methods. The spike already validated overload
   coexistence and every other matrix row.
3. STEP R3 - 8c dictionary [opus]. Collapse add/set conditional-consume bodies to
   `if (insertSlot == -1) return false;` + plain stores; delete `_placeOrInsertSink`,
   `_freeValue` (btree.cb also uses the pattern - update it), and the inner `is_unique(V)`
   duplicate-free. This is the first REAL exercise of 8a's exit drop (the refused-duplicate
   value must drop at scope exit, previously the leak that forced unconditional-move
   inference). New leak legs: add-duplicate refusal across every V kind.
4. STEP R4 - close-out. Confirm the remaining container `if const` inventory equals the
   "accepted permanent residue" list exactly (transfer-overload guards, copy() diagnostics,
   hash dispatch + key-policy rejections); 9c stays KEEP, 9d stays DEFER; full sweep
   (test.sh cold+warm, example_mac, test_lsp, leak oracle); then mark this plan COMPLETE.

Done and out of scope of the roadmap: Parts 1-6 shipped; Part 9a (release/return unification +
the incomplete-type deferred-dtor compiler fix), 9b (total .copy()) shipped. GATE STATUS: the
8c-first spike narrowed THE RULE's blast radius to "entry defers to T like stores already do";
maintainer has engaged the track (spike requested 2026-07-23) - explicit sign-off on landing
8a+8b is the only open decision.
STEP R1 DONE 2026-07-23 (uncommitted at report time). 8a+8b landed as one compiler change.
Mechanism: consume detection is STRUCTURAL - CollectConsumedStoreNames (MainListener.h ~929)
finds params consumed by a plain `=`/slot store, decl-init, or conditional `move` on at least
one path (descends conditionals/loops, not lambdas); ApplyOwningSinkInference flags them
IsOwningSink + IsConsumeInferredSink (new TypeAndValue field, serialized as "cis"). The
copyability decision is DEFERRED to OwningSinkConsumesConcrete (LLVMBackend.h ~3612), the
single discriminator all concrete-type consumers consult (call-site poison, laundering guard,
rvalue-temp ownership, borrow marking, 8a drop) - required because the ForwardRefScanner
cannot resolve copyability during the forward pass. A consume-inferred sink consumes ONLY a
non-copyable owner; copyable T stays a borrow (store is a copy) - zero delta for existing
code. 8a: a consuming sink param is marked IsOwningStruct at entry (createFunctionBlock
~2370), riding the EXISTING scope-exit/early-return destructor loops; no-op on moved-out
slots. Explicit `move T` exit-drop path unchanged (already sound - the "murky" dict
_freeValue case verified freed-once); _freeValue retirement deferred to R3. Pre-flight: NO
existing tests flipped; the 3 use-after-move err tests still fire. Tests: +11 oracle legs
(405->416, incl. conditional-consume both-paths and rvalue-temp-into-copyable-sink),
err_inferred_sink_use_after_move.cb (poison via inference alone, no containers/keywords),
scratch bag_probe validates R2's collapsed single body works end-to-end. Verified (also
independently re-run): test.sh Release 482/0/8 cold+warm, oracle 416 HeapAudit+asan clean
cold+warm, example_mac 35/0, test_lsp green, --init warm probe explicit. Deferred as future
enhancement: call-arg consume inference (case-b of THE RULE) - today that shape is already a
clean compile error (laundering guard), so it is sound to defer. Incidental pre-existing gap
found and filed: internal/issue/discard-move-global-string-leaks.md (`_ = move <global
string>` leaks; unrelated to sinks). NEXT: R2.
STEP R2 DONE 2026-07-23 (.cb-only, uncommitted). queue.enqueue / stack.push / list.add /
list.set each collapsed to a single body with the DIRECT store spelling
(`_data[slot] = value;`) - NOT via _placeAt: R1's deferred call-arg consume inference means
passing the param to _placeAt trips the ownership-laundering guard (clean compile error, as
designed), while the in-body store is what CollectConsumedStoreNames sees. _placeAt deleted
from all three files (no remaining callers). Transfer overloads and dictionary untouched.
Oracle legs added for the inference-driven consumed-caller and copyable caller-keeps paths
on all four methods (416->431). The 3 use-after-move err tests fire via inference now.
Verified (independently re-run): test.sh Release 482/0/8 cold+warm, oracle 431 clean
cold+warm, example_mac 35/0. NEXT: R3 (dictionary + _placeOrInsertSink/_freeValue/btree).
STEP R3 BLOCKED 2026-07-23 (attempt cleanly reverted; tree back at R2 state, baseline
re-verified). The dictionary collapse is NOT .cb-only: two compiler gaps found, both filed
in internal/issue/ with minimal repros preserved in scratch/.
(1) sink-param-thin-unique-exit-drop-gap.md - PRIMARY: `unique V*` is is_copyable, so R1's
consume-inference never IsOwningStruct-marks it and 8a's exit drop skips it; the duplicate
refusal `return false;` path LEAKS the refused value (non-copyable owning structs drop
correctly). This is exactly the murky-move-param case the 8a bullet prescribed making
DropValue-total and R1 deferred. Caller-contract check PASSED for the working rows (refusal
consumes non-copyable V, keeps copyable V - matches shipped).
(2) move-copyable-param-overload-uaf.md - SECONDARY, pre-existing UAF: `V x = move
<copyable param>` steals the CALLER's buffer when a sibling `move V` overload + `alias K`
param coexist (ASan repro matrix r3_mock4/mA/mB/mC).
btree.cb examined: its _freeValue usage is materially different (explicit `move V` overloads
releasing borrowed-node inline-array slots via `move n->values[i]` - load-bearing slot
clear); its collapse needs the same fix (1) plus borrowed-slot ownership recovery - separate
follow-on. R3 lands after fix (1) (and (2) if the typed-local spelling is wanted); every
other row of the collapse was verified HeapAudit-clean against shipped semantics.
STEP R3 DONE 2026-07-23 (uncommitted). Fix (1) authorized and landed as the compiler change the
8a bullet prescribed; the dictionary collapse re-landed with the bare `return false;` refusal
spelling (fix (2) NOT needed - the bare-return path never uses the typed-local; gap 2 stays filed).
COMPILER FIX (1), two edits: (a) createFunctionBlock (LLVMBackend.h ~2275) computes uniqueAutoSink
= IsUniqueTypeArg && !IsAlias && !IsBorrowOfUniqueElement, and routes such a PLAIN (non-move)
unique-type-arg param through the owning branches - the interface-value arm sets IsOwning =
IsMove || uniqueAutoSink (EmitOwningInterfaceCleanup at exit), the pointer branch condition becomes
(IsMove || uniqueAutoSink) && Pointer (EmitOwningPtrCleanup at exit) - mirroring exactly the
call-site null condition (ApplyMoveParamTransfer line ~10409 nulls the caller on IsUniqueTypeArg),
so entry-ownership == caller-consumed and no path double-frees. (b) Part 6 slot-store (MainListener.h
~10393) gained a `unique <interface>` element arm: the thin-pointer arm already nulled the source on
move-out, but the interface fat-ptr store fell through WITHOUT nulling, so once the param was owning
its scope-exit freed a value already handed to the slot (ASan double-free in _placeValueAt). The new
arm stores the fat value then ConstantAggregateZero + MarkVariableMoved the named source, so the
moved-out slot is a no-op at exit. No new serialized field (uniqueAutoSink is a local; IsOwning/
IsUniqueTypeArg already ride the cache) - warm-cache oracle + err tests re-verified explicitly.
Pre-flight: grep found no by-value unique-param body that hand-frees + relies on no exit drop (only
comments and btree node deletes); full suite green pre-collapse confirms zero flip. Unique-INTERFACE
sinks ARE covered (arm (b)). DICTIONARY COLLAPSE: single-body add (`if (insertSlot == -1) return
false;` + direct `_values[slot] = value`) and set (direct store, overwrite via _releaseValueAt);
deleted _placeOrInsertSink, _freeValue, _placeValueAt (now unused), the inner is_unique(V)
duplicate-free, and both is_copyable(V) add/set if-const splits. Kept: the `!is_pointer(V)` transfer
overloads, the _slot key-policy compile_error guards, copy()'s dead-branch is_unique/is_copyable
guards. btree.cb UNTOUCHED (follow-on now has fix (1) but still needs borrowed-slot recovery).
Tests: +21 oracle legs (431->452: dup-refusal across primitive/string/copyable-struct/non-copyable-
holder/thin-unique/unique-iface incl. NAMED sources, plain-set non-copyable overwrite, and a
container-free gap-1 shape `_r3UniqueSink`). Verified: test.sh Release 482/0/8 cold+warm, oracle 452
HeapAudit+asan clean cold+warm, example_mac 35/0, test_lsp 152/0, dictionary err tests fire cold+warm.
issue sink-param-thin-unique-exit-drop-gap.md DELETED (fixed); move-copyable-param-overload-uaf.md
KEPT (gap 2, separate session).
STEP R4 DONE 2026-07-23 - PLAN COMPLETE. Residual container `if const` inventory verified
equal to the accepted-permanent-residue list: 5x `!is_pointer` transfer-overload guards
(list add/set, stack push, queue enqueue, dictionary set), the copy() diagnostic ladders
(is_unique dead-branch guards + tailored !is_copyable messages in list x2 / dictionary),
and dictionary _slot hash dispatch + the is_unique(K) key-policy rejection. Every OWNERSHIP
decision - store, release, return, copy, and (new in Part 8) function ENTRY - defers to T
with zero `if const`. 9c stands as KEEP, 9d as DEFER. Final sweep (independently re-run):
test.sh Release 482/0/8, leak oracle 452 HeapAudit clean, example_mac 35/0, test_lsp 152/0
(agent-run post-R3). Open follow-ons moved to the plan header. Whole R1-R4 changeset is one
uncommitted working tree for maintainer review.
GATE RESOLVED 2026-07-23: maintainer signed off via design principle rather than a literal
yes - "CFlat is alias/borrow by default (between C++ copy-default and Rust move-default);
the goal is easy, memory-safe, performant user code, OFFLOADING the copy/move decision to
the compiler." That principle selects inference (8b) over annotation, accepts the
caller-side use-after-move errors as the compiler doing its job, and endorses the 8a
exit-drop (no value falls on the floor). R1 may proceed.

## Status

- 2026-07-22: probe matrix + code survey done; four soundness issues filed. Parts 1-3 are
  unconditional wins (bug fixes + refactor) and can proceed independently of the Part 4
  decision. Part 4 awaits maintainer sign-off on the copy-on-assign flip.
- 2026-07-23: Parts 2-3 landed (all four unique-* soundness issues fixed + deleted; commits
  ff55e6b/4d2a80d/ee8d279/454bf50). Part 4 THE FLIP SIGNED OFF by maintainer and SHIPPED:
  copyable owners (string, closures already did, containers, structs with copy()) now COPY
  on a NAMED-source `=` across all four contexts (4a local reassign, 4b decl-init, 4c field
  + brace-init store, 4d deref store); non-copyable owners keep move-with-consume; borrows and
  temp/call-result sources unchanged (xml.cb-style field borrows still alias). Predicate is the
  shared `IsCopyableType` (LLVMBackend.h), which the is_copyable intrinsic now also calls; the
  copy is emitted by `EmitCopyableOwnerCopy` (string -> deep copy, else copy() overload/synth).
  Test deltas: 4 now-legal err tests deleted (err_container_copy_into_field, err_field_to_field_copy,
  err_string_field_alias_via_local, err_string_owned_local_into_field); positive `testCopyOnAssignFlip`
  added to test_move.cb; deref-assign + a field-to-field value-struct leg added to test_collection_leaks.cb.
  REVIEW FIX (not in the agent's first pass): copyable value-STRUCT field-to-field (`b.f = a.f`)
  double-copied (RejectOwningValueCopyIntoField AND the dedicated field-to-field block both copied),
  orphaning the first copy = leak; fixed by threading an `outCopied` flag so the field-to-field block
  only copies when Reject bailed. Regression leg added under the HeapAudit oracle.
  Verified: test.sh 474/0/8, example_mac.sh 35/0, test_lsp.sh green. Remaining: Part 5 (unify the
  hand-mirrored assignment paths) and Part 6 (container slot-store payoff) are the follow-ons.
- 2026-07-23: Parts 1, 5, 6 all landed (uncommitted working tree at report time; Part 4 was
  committed separately as 397ec21). Suites green throughout: test.sh 476/0/8 (cold+warm),
  example_mac.sh 35/0, test_lsp.sh 152/0.
  - PART 1 DONE. `DropValue(const NamedVariable&)` extracted from EmitDestructorsForScope (pure
    verbatim refactor, zero delta) - the per-local release ladder is now one reusable method the
    scope loop calls. Its follow-on explicit-release form is `_ = move <bare-local>;` (the canonical
    spelling). It is intercepted in the `_ =` discard branch of ParseAssignmentExpression when the
    RHS is a top-level `move` over a bare identifier naming a live local; it releases the local via
    DropValue, nulls the slot, marks it moved (use-after = error). SPELLING DECISION (2026-07-23):
    `_ = move` is the ONE spelling. Promoting it also fixed two latent leaks in the old
    generic discard path (`_ = move` of a thin `unique T*` or owning-struct local leaked) and added
    the missing moved-from marking. LIMITATION: only a bare-name local; NOT a subscript lvalue - that
    gap is why Part 6 STEP D was deferred. Coverage: test_collection_leaks.cb leg +
    err_discard_move_use_after.cb.
  - PART 5 DONE (STEP 1; STEPS 2-3 deliberately deferred). The copy-vs-move source classification
    (where the Part 4 bugs lived) is now ONE shared helper `ClassifyOwningAssignSource` routed
    through decl-init, reassign, and deref - the hand-mirroring of that decision is gone. STEP 2
    (route drop-old through DropValue) was NOT done: DropValue's alias/borrow/string-owned-bit skips
    DIFFER from the current unconditional destruct-old, so swapping is not provably zero-delta (it
    may even fix a latent double-free on reassign-over-alias) - left specialized per the plan's
    "resists unification, leave it" clause. STEP 3 (field-store fold) already shares
    RejectAliasStoreIntoField/RejectOwningValueCopyIntoField; the rest is genuinely path-specific.
  - PART 6 DONE - MAXIMAL insert-side collapse achieved. The single-index-GEP exclusion is LIFTED
    (MainListener.h, new gated block ~10186): a `_data[i] = value` store of a NAMED owning source
    (alloca/global-backed param/local) into a container slot now defers to T via the shared
    ClassifyOwningAssignSource/EmitCopyableOwnerCopy/CloneClosureFromNamedSource - COPY a copyable
    owner, MOVE+consume a non-copyable/unique, CLONE a closure. So `_placeAt`/`_placeValueAt`
    collapsed to a SINGLE LINE `_data[index] = value;` (zero if const) in queue/stack/list/dictionary
    - the 5-way is_unique/is_pointer/is_primitive/!is_copyable ladder is gone (if const per container:
    queue/stack 11->7, list 20->16, dictionary 32->28). KEY SOUNDNESS FINDING (hit the hard gate,
    fixed soundly not reverted): the lifted slot store must NOT drop-old - a container writes only
    into a DEAD/EMPTY slot (set() calls _releaseAt first), and drop-old there double-destructs the
    default-constructed slot (a `list<Val>` with a side-effecting dtor proved it: count 3 != 2).
    Removing drop-old fixed it and also mooted the interface-slot-not-zeroed hazard (garbage slots
    are overwritten, never read). Gated to owning element types so raw non-owning arrays and
    grow-loop GEP-source moves (`newData[i] = move _data[j]`) fall through untouched. Leak matrix
    333/333 HeapAudit-clean across every element kind (int, POD, string, Holder{unique R*}, R*,
    unique R*, IShape, unique IShape).
  - STILL if const (accepted): the enqueue/add METHOD-LEVEL is_copyable split (it controls SINK-ness
    - whether the caller's arg is consumed - via `move value` in the non-copyable arm, not store
    semantics; keeping it is the plan's accepted sink-inference fallback), and `_releaseAt`/`_freeValue`
    (Part 6 STEP D, the teardown-side explicit-release collapse, deferred - needs `_ = move` extended
    to a subscript lvalue, a separate riskier codegen change). These are the only remaining container
    if const.
  - OPEN FOLLOW-ONS (not blocking): Part 5 STEP 2 (DropValue-based drop-old unification, which may
    surface/fix a latent reassign-over-alias double-free); Part 7 (member-wise move hook, from the
    sibling plan).
- 2026-07-23 (Part 5 STEP 2 INVESTIGATED - recommend NOT doing the unification; no correctness
  payoff). Probed the "may fix a latent reassign-over-alias double-free" hypothesis with a HeapAudit
  probe. The double-free is REAL but its source is NOT the drop-old scatter STEP 2 would touch: a
  plain `Res b = a;` (no alias, no reassign) of a value struct with a USER destructor over a RAW
  pointer (no `unique`, no `copy()`) shallow bit-copies -> two owners -> double-free. Root cause is
  `IsCopyableType` mis-classifying such a struct as copyable (only checks `TypeOwnsUniquePointer`,
  blind to raw-pointer-with-user-dtor); `ClosureCaptureDeepCopyable` already guards the identical
  hazard. Filed `internal/issue/copy-of-user-dtor-raw-pointer-struct-double-frees.md` (FLIP-adjacent,
  needs delta enumeration + sign-off). CONCLUSION: STEP 2 routing drop-old through DropValue would
  NOT fix this and has no other correctness payoff - it is pure cosmetic unification of very delicate
  code with a real regression risk. Per the plan's "resists unification, leave it" clause, STEP 2
  stays deferred. Also filed `internal/issue/discard-move-vs-decl-move-element-ownership-disparity.md`
  (the `_ = move <element>` vs `T tmp = move <element>` gap from Part 6 STEP D).
- 2026-07-23 (STEP D DONE - collapse via `T tmp = move`, NOT `_ = move`). The `_ = move _data[i]`
  route was ABANDONED after a design probe: container element ownership (`unique` vs borrow) is a
  GENERIC-TYPE-BINDING property (only in activeTypeSubstitutions, what is_unique(T) reads), ABSENT
  from the element lvalue - `queue<unique Box*>` and `queue<Box*>` share a byte-identical `Box**`
  buffer and a slot read demotes to a bare `Box*` (IsUnique=0). A `_`-discard has no destination type
  to recover ownership from, so it silently stripped owned elements -> leak. Fable adjudicated the
  fork: use a real `T tmp = move _data[i];` local (a T-typed decl DOES carry ownership via
  substitution) and FIX the underlying move-out-into-a-dropped-local bug (the plan's "Stage 3
  enabler"; issue move-out-into-dropped-local-ownership.md now DELETED). Root cause: two element-move
  branches in ParseMoveExpression conferred ownership blind to the demoted element kind - the pointer
  branch set lastOwningResult unconditionally (bare `T*` element -> spurious delete -> double-free),
  the interface branch set nothing (`unique <iface>` element -> under-owned -> leak). Fix: a compiler
  flag `lastMovedFromContainerSlot` (set on an IsElementAccess move, reset at ParseAssignmentExpression
  entry AND in ResetForReanalysis) gates a decl-init override that RE-DERIVES the dropped local's
  ownership from the DESTINATION type (IsUniqueTypeArg): unique thin-ptr / unique interface -> owning;
  bare ptr / bare interface -> non-owning; value/string untouched. Strictly gated to element-slot
  sources, so `Box* b = move owned;` (named local) stays source-keyed and the dequeue RETURN path
  (returnedStructDtorSkipAlloca) is unaffected. Collapsed queue/stack/list `_releaseAt` and dictionary
  `_releaseValueAt` to one line (array.cb + dictionary KEY release left as-is). Verified: leak matrix
  352/352 (19 new in-body-drop legs across every element kind), test.sh 476/0/8, example_mac 35/0,
  test_lsp 152/0. Only remaining container if const: the enqueue/add is_copyable SINK split (accepted).
  Committed as c396327 (squash of Parts 1/4/5/6 + STEP D; the separate Part 4 commit 397ec21 was
  folded in by rebase).
- 2026-07-23 (IsCopyableType double-free FIXED; commit b1c52c9). `IsCopyableType` (LLVMBackend.h) now
  rejects a value struct with a USER destructor over a raw pointer/view field and no HAND-WRITTEN
  copy() (new `StructSynthCopyUnsafe` helper, gated by `!HasRealCopyOverloadFor` so
  string/closures/containers with a real copy() are unaffected; mirrors the guard
  `ClosureCaptureDeepCopyable` already applied for closure capture). Such a type is now non-copyable,
  so `T b = a;` MOVES (single owner) and a field store gives the actionable ".copy()/move" error - no
  more silent shallow-copy double-free. Regression: `Test/test_collection_leaks.cb` RawResLeak (move,
  one free) + RawResCopyable (real copy(), two independent frees) under HeapAudit. Issue file deleted.
- 2026-07-23 (`_ = move <element>` disparity FIXED; commit b1c52c9). SUPERSEDES the STEP D "abandoned"
  verdict on the discard spelling: `_ = move _data[i]` now releases an owning container element
  exactly like `T tmp = move _data[i]`, via ONE shared recovery + the existing DropValue (no bespoke
  per-kind release codegen). Mechanism: the buffer field `T* _data` (T = `unique X*`/`unique IFace`)
  has its element `unique`-ness stripped by the explicit-pointer rule (slot reads stay borrows); a
  new `TypeAndValue.ElementOwningUnique` flag (LLVMBackend.h; `--init` key `"eou"` added both
  directions) preserves it so the move site can re-derive. Shared helper `ApplyMovedSlotOwnership`
  (extracted from the decl-init recovery block, behavior-preserving there) drives both spellings; the
  `_ =` discard branch materializes the detached value into a temp and DropValue-frees it (owning
  ptr/iface/value/string once; borrow nothing). En route it fixed two bugs the discard path had for
  slot moves: over-freeing a borrow pointer element and leaking a string element (both from the old
  RegisterDiscardedOwningStructTemp fallthrough). Regression: `DiscardBag<T>` in
  test_collection_leaks.cb releasing via `_ = move _data[i]` in both discardAll AND its destructor,
  over every element kind (leak matrix now 374, HeapAudit-clean). Suites green (independently
  re-verified): test.sh 476/0/8, example_mac 35/0, test_lsp 152/0. Issue file deleted. Both follow-on
  issues from the STEP 2 investigation are now resolved.
- NET STATE after commits c396327 + b1c52c9: BOTH release spellings are sound and DropValue-backed -
  `_ = move <bare local>` and `_ = move _data[i]` (slot), as well as `T tmp = move _data[i]`.
  Remaining open follow-ons: Part 5 STEP 2 (recommended NOT to do) and Part 7 (member-wise move
  hook, sibling plan).
- 2026-07-23 (containers switched to the discard spelling; uncommitted spike, .cb-only). The core
  containers' `_releaseAt`/`_releaseValueAt` (queue/stack/list `T tmp = move _data[i];`, dictionary
  `V tmp = move _values[i];`) now use `_ = move <slot>;` - the original Part 6 spelling, viable since
  the b1c52c9 disparity fix; no compiler change needed. The warm-cache risk (core code hitting the
  discard-slot-move path through `--init` bitcode, exercising the "eou" round-trip) was explicitly
  probed: full leak matrix (374) HeapAudit-clean against a warm cache. Suites green: test.sh Release
  476/0/8 (cold+warm), example_mac 35/0, test_lsp 152/0.
- 2026-07-23 (evening): 9a COMPLETED including the compiler fix (EmitOwningPtrCleanup resolves
  through GetFullDestructorForDelete - the live self-ref leak through dict/stack/queue teardown
  is fixed, ~list() collapsed, 6 self-ref regression legs in test_move.cb, issue file deleted).
  9b DONE (total `.copy()` over T; mechanism and collapses in the 9b section). 8c-first SPIKE
  run and restored (record in the Part 8 section): the single-body insert surface works for
  every matrix row except the non-copyable owning VALUE struct, which compiles, skips
  poisoning, and DOUBLE-FREES - store defers to T, entry does not; minimal delta confirmed as
  8b (move-classified consume inference) + 8a (scope-exit DropValue invariant). Roadmap
  section (R1-R4) added before Status. Suites green at every step: test.sh Release 480/0/8
  (cold+warm), leak oracle 405 under HeapAudit, example_mac 35/0, test_lsp green.
