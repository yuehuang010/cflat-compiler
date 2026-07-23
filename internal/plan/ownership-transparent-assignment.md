# Ownership-transparent assignment: `dest = src` defers to T

Opened 2026-07-22. GOAL, multi-change, iterative. This is "Option 5" from the container
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
- BONUS: this helper is the `drop <lvalue>;` primitive from the container brainstorm
  (Option 1) - exposing it as a statement afterward is a small grammar+ParseStatement change
  and collapses every `_releaseAt`/`_freeValue` ladder. Track that as its own follow-on.
- Files: `cflat/LLVMBackend.h`. Verify: all suites green, zero test deltas.
- Depends: none.

### Part 2 - Total drop-old-destination on reassign   [opus, medium risk]
- Route the five scattered destruct-old blocks through DropValue and COVER THE HOLES:
  unique-LOCAL reassign frees the old pointee (fixes the leak issue), unique FIELD store
  stops silently aliasing (fixes the unguarded-alias issue; either consume the source or,
  interim, reject the plain form requiring `move`).
- Ordering rule everywhere: produce/clone the source value BEFORE dropping the old dest
  (the closure block is the pattern); keep the `destination != source` self-assign guard.
- Files: `cflat/MainListener.h` (ParseAssignmentExpression), `cflat/LLVMBackend.h`.
  Extend `Test/test_collection_leaks.cb` reassign legs; new `Test/errors/err_*` as needed.
- Verify: both issue repros green under HeapAudit; suites green.
- Depends: 1. Deletes: unique-local-reassign-leaks-old-pointee.md,
  unique-field-store-unguarded-alias.md (delete the issue files when fixed).

### Part 3 - Unique edge unification   [opus, low-medium risk]
- Fix the self-assign segfault (guard or reject - decide at implementation; rejection
  matches existing unique diagnostics).
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
  to a plain store, and `_releaseAt`/`_freeValue` to `drop` (from Part 1's follow-on).
- Two things to prove here:
  - drop-old on a DEAD slot is a no-op (containers zero moved-out slots; the leak matrix
    must show teardown/insert-over-dead-slot clean for every element kind);
  - sink inference interplay: with no literal top-level `move value` in the collapsed
    insert body, either extend inference to count "param assigned with move semantics" as
    sink-marking, or keep the single `is_copyable(T)` method split as the one surviving
    `if const` (acceptable fallback - it is the intended policy split anyway).
- Files: `cflat/MainListener.h`, `cflat/LLVMBackend.h`, `cflat/core/{queue,stack,list,dictionary}.cb`,
  `Test/test_collection_leaks.cb`.
- Verify: full leak matrix green; container `if const` count drops to the policy split only.
- Depends: 4, 5. Also unblocks re-visiting the read-side
  `internal/issue/move-out-into-dropped-local-ownership.md` via `drop` instead of
  move-into-dropped-local.

### Cross-cutting constraints (every part)
- Any change to type parsing goes in BOTH `ParseDeclarationSpecifiers` copies. Errors via
  `LogError`/`LogErrorContext` only. ASCII only. Inline comments <= 2 lines.
- Any new field on `TypeAndValue`/`StructData`/`AnnotationValue` that an analysis reads MUST
  be added to the `LLVMBackend.cpp` `--init` cache round-trip in the SAME change.
- Do NOT create new test files except `Test/errors/err_*.cb`; extend existing tests.
- Verify on the current host (`test.sh Release`, `example_mac.sh`, `test_lsp.sh`) before
  declaring a part done; never dilute an assertion; never convert a compile error into a
  silent double-free.

## Status

- 2026-07-22: probe matrix + code survey done; four soundness issues filed. Parts 1-3 are
  unconditional wins (bug fixes + refactor) and can proceed independently of the Part 4
  decision. Part 4 awaits maintainer sign-off on the copy-on-assign flip.
