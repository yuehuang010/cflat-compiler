# Borrowed-slot ownership recovery: retire btree.cb's `_freeValue` pattern

Opened 2026-07-23 as the follow-on from [[ownership-transparent-assignment]] (STEP R3 record:
"btree.cb UNTOUCHED (follow-on now has fix (1) but still needs borrowed-slot recovery)").
COMPLETE 2026-07-23: Stage 1 landed as 1eeea60 (guard #2 gains `&& !argNV.IsElementAccess`),
Stage 2 as 629015f (btree collapse), both merged fast-forward. See "Completion record" at the
end for the two deviations from this plan discovered during implementation.
GOAL, one compiler change + one library collapse. Related: [[container-ownership-transparency]]
(own-slot vs caller-source distinction), commits 1c6eae6 (borrowed copyable move degrades to
copy), 009fcdb (`_ = move <global>` discard-release), and the R1-R4 sink-inference track.

## 1. Problem statement

The R1-R4 work made every ownership decision - store, release, return, copy, function entry -
defer to the element type T, and collapsed the `if const` ladders out of queue/stack/list/
dictionary. btree.cb (`cflat/core/hpc/btree.cb`) could not join because its release sites free
value slots reached through BORROWED node pointers (`btree_node<K,V>* n` params, borrows into
a tree whose nodes are only ever reachable through other borrows), and the compiler rejects
`move n->values[i]` through a borrowed base:

- MainListener.h ~14632 (ParseMoveExpression, guard #2): `!argNV.OwningStructName.empty() &&
  argNV.IsBorrowed && !argNV.BorrowedOrigin.empty()` -> "cannot 'move' field '{}.{}' through
  borrowed parameter '{}'". Any non-move pointer param is IsBorrowed with BorrowedOrigin =
  its own name (LLVMBackend.h createFunctionBlock ~2397-2401); field access carries the taint
  (MainListener.h ~15438-15445: OwningStructName, IsBorrowed, BorrowedOrigin); the subscript
  marks IsElementAccess (~16805) but clears none of the provenance. The guard does not
  distinguish a whole field from an element slot, nor a pointer base from a by-value copy.

btree works around it with the `_freeValue(move V)` / `_freeKey(move K)` empty-body helpers
(release-by-parameter-scope-exit), `_clearValue`/`_clearKey` abandon-clears, `_destroyKey`
in-place destruction, and 4-arm `if const` gating at every call site - exactly the shape the
other containers retired.

## 2. Site inventory: every `_freeValue` / explicit-`move`-param / clear workaround in btree.cb

Legend for "needs": **A** = nothing new - the node is a genuine local or `move` param, or the
R3 gap-1 compiler fix (callee entry ownership mirrors call-site consume for unique-type-arg
params) already covers it; collapsible today, verify by probe. **B** = needs the borrowed-slot
recovery (the compiler change in this plan). **C** = optional simplification enabled by the
synthesized node destructor (frees all owning slots since 2026-07-20).

| site (btree.cb) | lines | today | target form | needs |
|---|---|---|---|---|
| `_freeValue(move V)` / `_freeKey(move K)` | 244-256 | empty-body scope-exit release helpers | DELETE (all callers rewritten) | A/B |
| `_clearValue` / `_clearKey` | 202-210 | abandon-clear after shallow copy | DELETE (sources become slot-to-slot `= move`) | B |
| `_destroyKey` | 215 | in-place `.~()` through borrowed parent | `_ = move n->keys[i];` | B |
| `add()` duplicate refusal | 483-491 | `if const (is_unique(V)) _freeValue(move value)` | bare `return false;` (gap-1 exit drop, mirrors dictionary) | A |
| `insert()` duplicate refusal | 774-779 | same, plus lock release | bare `return false;` after `release()` | A |
| `set()` overwrite ladder | 509-516 | 4-arm `if const` + `_freeValue(move n->values[s])`, n LOCAL | `_ = move n->values[s];` | A (probe) |
| `set(move V)` overwrite | 551-554 | `_freeValue(move n->values[s])` | `_ = move n->values[s];` | A (probe) |
| `remove()` entry release | 944-952 | 4-arm V ladder + K gate, leaf LOCAL | `_ = move leaf->values[i]; _ = move leaf->keys[i];` | A (probe) |
| `remove()` down-shift | 954-962 | `keys[j] = move keys[j+1]` (leaf local) | unchanged - already the transparent form | - |
| `_splitChild` leaf half-transfer | 321-342 | shallow copy + `_clearKey/_clearValue` (left reached via borrowed parent) | `right->keys[i] = move left->keys[mid+i];` (and values) | B |
| `_splitChild` internal keys + median | 344-366 | shallow copy + `_clearKey`; `_dupKey` + `_destroyKey` for the median | key transfer by `= move`; median: `_insertChildAt(parent, idx, move left->keys[mid], right)` if the call-arg element-move form probes clean, else `_dupKey` + `_ = move left->keys[mid]` | B |
| `_insertChildAt` / `_leafOpenSlot` rotations | 289-305 / 391-407 | shallow rotate into empty tail | unchanged (bit-relocate invariant; see Risks) | - |
| `_placeValue` ladder + `_leafInsertNew` poisons | 269-280, 418, 475, 503, 770 | 5-arm store ladder + copies of the copy() diagnostic | `n->values[i] = value;` (Part 6 store semantics; keep ONE tailored `!is_copyable` diagnostic per entry point, dictionary-style dead-branch) | A |
| `_borrowFromLeft` / `_borrowFromRight` | 1160-1237 | shallow take + `_clear*` + `_destroyKey`/`_storeKey` | vacating takes become `= move src->slot`; separator drop `_ = move parent->keys[idx]` then `_storeKey` (keep - key-copy policy is deliberate) | B |
| `_mergeWithLeft/Right` parent fixup | 1069-1082, 1127-1138 | shift + `_destroyKey` + `_clearKey` | separator drop `_ = move parent->keys[..]`; down-shift as `parent->keys[m] = move parent->keys[m+1]` (self-clearing, like remove()) | B |
| `_mergeWithLeft/Right` entry transfer | 1032-1043, 1091-1101 | already `move node->keys[k]` (node is a `move` param) | unchanged | - |
| `_freeSubtree` leaf loop | 1263-1289 | 4-arm V ladder + K gate via `_freeValue`/`_freeKey`, n is `move` param | either `_ = move n->values[i]` (A) or delete the loop entirely - the synthesized node destructor already frees every owning slot and leaves borrows alone, so `_freeSubtree` = recurse children + `delete n` | A / C |

Kept on purpose (accepted residue, matching R4's inventory): the `!is_pointer(V)` transfer
overloads (`add/set/insert(K, move V)`), `_storeKey`/`_dupKey` key-copy policy ladders,
`_lt/_le/_eq`, the concurrent-API machinery, and tailored `compile_error` diagnostics.

## 3. Compiler gap or soundness question?

**It is a compiler gap.** Trace of what the machinery actually requires:

- `ApplyMovedSlotOwnership` (MainListener.h ~7457) requires NOTHING of the base. It re-derives
  the detached value's ownership purely from the element's TypeAndValue: `IsUniqueTypeArg ||
  IsUnique || ElementOwningUnique` x (thin ptr | unique iface) -> owning; bare ptr/iface ->
  ownership cleared. Base requirements are enforced entirely upstream, by guard #2.
- The move mechanics already write through to the ONE REAL SLOT for a pointer base: the
  eager element paths in ParseMoveExpression null/zero `argNV.Storage`, which is a GEP through
  the loaded node pointer - thin-pointer null at ~14800, interface fat-value zero at ~14691,
  value-struct/string aggregate zero at ~14753 - and set `lastMovedFromContainerSlot` so the
  destination (`T tmp = move ...`, decl-init override ~8547) or the discard site
  (`_ = move ...`, ParseAssignmentExpression ~9449, materialize + DropValue) recovers
  ownership. All of this is shipped and exercised daily by dictionary's `_releaseValueAt`
  (`_ = move _values[i];`) - whose base is `this`, a borrowed pointer in every practical
  sense that the guard simply does not taint.
- The double-free hazard guard #2's message describes is real ONLY for the by-value-copy
  shape: `move h.p` on a borrowed by-value struct param nulls the CALLEE'S COPY while the
  caller's original still owns (guard #1, ~14617, `IsBorrowedStructParameter`). For a
  POINTER base there is no copy: the null is globally visible, the node destructor sees an
  empty slot, every other alias reads default(V). Memory-safe by the same argument as the
  local-base form. Note also the incoherence that proves the boundary is accidental: a
  DIRECT subscript of a borrowed pointer param (`move p[i]`) never trips guard #2 at all
  (OwningStructName is empty), and a laundered `V* vals` local even gets ElementOwningUnique
  stamped (MainListener.h ~3085) so recovery would work - only the `n->field[i]` spelling is
  blocked.

**The residual soundness obligation is a library invariant, not new static machinery**: "this
slot currently holds a value the tree exclusively owns, and I am entitled to release it." That
is exactly the invariant dictionary/queue discharge for their own `_data[i]`, and exactly the
one btree already discharges at its local/move-param sites today. Aliased node pointers do not
weaken it, because the move CLEARS the slot - a second release through any alias is a no-op,
never a double-free. Concurrent access is governed by btree's existing contract (single-
threaded mutation family, or exclusive `olock` held) - a slot move is a plain store through a
borrow, the same as `_placeValue` already performs, so hand-over-hand locking and the future
concurrent B-link tree need NO exclusive static aliasing. No Rust-style borrow checking enters
the design.

## 4. Candidate approaches

### Approach 1 (RECOMMENDED): narrow guard #2 - element slots through borrowed pointer bases are movable

In ParseMoveExpression, exempt element access from the borrowed-parameter field-move
rejection:

```cpp
if (!argNV.OwningStructName.empty() && argNV.IsBorrowed
    && !argNV.BorrowedOrigin.empty() && !argNV.IsElementAccess)   // <- new conjunct
```

Rationale for the predicate being exactly this narrow:
- `IsBorrowed` is only ever set on non-move POINTER params and locals aliasing them
  (LLVMBackend.h ~2397; by-value owning params get `IsBorrowedOwningValue`, a different flag)
  - so `IsBorrowed && IsElementAccess` implies the slot GEP dereferences the real object.
- The by-value copy shape stays rejected by guard #1 (`IsBorrowedStructParameter` keyed on
  `ParentVariableName`, which the subscript retains) - `move byValParam.values[i]` still
  errors.
- Whole-field moves through borrows (`move n->next`, `move c->item`) stay rejected - both
  existing err tests (err_move_field_borrowed_param.cb,
  err_move_field_out_of_borrowed_struct_param.cb) assert whole-field shapes and keep firing.
- Downstream, nothing else is needed: for btree's INLINE arrays (`V values[16]`), the field
  declaration has no explicit star, so `IsUniqueTypeArg` is never demoted (the ~3085 demotion
  applies only to `T* _data`-style buffers) and rides onto the element through the fixed-array
  subscript branch (~16700, which keeps the field's TypeAndValue) - `ApplyMovedSlotOwnership`'s
  `unique` disjunct is satisfied without touching `ElementOwningUnique`. No new serialized
  state.

Trade-offs: this is a small language-semantics extension - all user code gains
`move p->arr[i]` through borrowed pointers, with recovery semantics. That is philosophy-
aligned (borrows are mutable; the compiler infers ownership from T; `this`-based slots
already behave this way) but the blast radius must be enumerated (see Risks).

### Approach 2: btree-side restructuring only (no compiler change)

Route every release through an owning access path. Rejected as the primary approach because
no such path exists: B-tree nodes are only ever reachable through borrows (parent
`children[i]`, leaf `next` chain), `move` params free at scope exit so a helper cannot borrow-
take-and-give-back, and hoisting releases to sites where the node is a local is precisely the
contortion btree already performed (`_descendSplitting` exists solely so `set()` holds the
leaf as a local - line 437's comment). The one genuinely new .cb-only trick - laundering
through a direct pointer subscript (`V* vals = &n->values[0]; _ = move vals[i];`), which
evades guard #2 because OwningStructName is empty and even gets ElementOwningUnique stamped -
works by ACCIDENT of the guard's shape and would silently break if the guard is ever
tightened. Use it only as a Stage-0 validation probe, never as the shipped form.

### Approach 3: explicit ownership-recovery construct (`release n->values[i]` / an `owned` cast)

Rejected. CFlat's design principle (recorded at the R1 gate resolution: borrow-by-default,
the compiler owns the copy/move decision, no user-facing ownership annotations) makes a new
annotation a last resort, and Approach 1 shows inference suffices - the element type plus the
existing recovery flags already carry every bit the compiler needs.

**Recommendation: Approach 1**, with Approach 2's laundering trick used once as a pre-change
probe to validate the downstream machinery on btree's exact shapes.

## 5. Risks

- **Double-free shapes.**
  (a) Moved slot + synthesized node destructor: the eager zero (thin null ~14800, fat-value
  zero ~14691, aggregate zero ~14753) makes the destructor's walk a no-op - same measured
  mechanics `_freeValue`'s comment documents. Verify per element kind under HeapAudit,
  including `unique IShape` (fat) and a struct owning a `unique` field (the `.~()` non-
  substitute case from the `_freeValue` comment).
  (b) Rotations: `_insertChildAt`/`_leafOpenSlot` shallow-rotate into an EMPTY tail with the
  vacated slot immediately overwritten - keep them as plain stores. Do NOT touch slot-store
  classification for GEP/element SOURCES: the plan's LOAD-BEARING INVARIANT (dictionary rehash,
  list sort) depends on element-source plain stores staying bit-relocations. This design
  changes only the `move` GUARD, not store classification - re-state this in the change.
  (c) Converted `= move` slot-to-slot transfers must only replace copy+clear pairs, never a
  copy whose source is deliberately preserved (`_dupKey` separators, `_storeKey` rewrites stay
  copies).
- **R1-R4 interaction.** A body containing `_ = move n->values[i]` must NOT cause `n` to be
  consume-inferred as a sink (`CollectConsumedStoreNames` collects param NAMES consumed;
  element accesses are excluded from move tracking and no `MarkVariableMoved(n)` fires on the
  eager element paths) - node params must remain borrows or hand-over-hand locking dies. Add
  an explicit probe: caller's node pointer stays readable after calling a releasing helper.
  Commit 1c6eae6's degrade-to-copy is keyed on whole borrowed by-value params
  (`IsVariableBorrowedOwningValue`, and its check at ~14678 already excludes IsElementAccess) -
  slot moves keep genuine move semantics; no interaction. 009fcdb (global discard) is disjoint.
- **Delete-borrow discipline.** `V taken = move n->values[i]` for a BARE-pointer V must still
  reject `delete taken` (recovery clears IsOwning/IsNewAllocated; the stack `alias T*`
  precedent says this holds) - add an err leg.
- **Guard-relaxation blast radius.** All user code gains element moves through borrowed
  pointers. Enumerate before landing: grep err tests for the guard #2 message (2 files, both
  whole-field - no flips expected); grep for `move` of subscripted borrowed bases in
  Test/example (expected none, previously a compile error so no compiling program changes
  behavior - the Part 4 discipline).
- **Serialized state / `--init`.** NONE needed: the change is a parse-time guard predicate;
  recovery reuses `ElementOwningUnique` ("eou", already round-tripped, LLVMBackend.cpp
  3817/3877) and `IsUniqueTypeArg`. Because btree.cb is core-library code, still run the leak
  legs against a WARM cache explicitly (the R2/R3 discipline) to prove the inline-array
  IsUniqueTypeArg retention survives the round-trip.
- **Concurrent API.** lookup()/insert() never release, so the collapse cannot perturb the
  speculative-descent contract; but insert()'s duplicate-refusal collapse (bare `return false`
  after `leaf->ver.release()`) moves the refused `unique` value's free from an explicit call
  to scope exit - confirm the free happens AFTER the lock release either way (it does: scope
  exit is after both), and keep the "no side effects in speculative regions" review rule
  untouched.

## 6. Staged implementation plan

**Stage 0 - probes (read-only, scratch + HeapAudit).** (a) Confirm the A-rows collapse today:
`_ = move n->values[s]` at set()/remove()/_freeSubtree (local / move-param bases) across V in
{int, string, copyable struct, Holder{unique R*}, R*, unique R*, IShape, unique IShape};
verifies inline-array IsUniqueTypeArg retention end-to-end. (b) Confirm guard #2 is the ONLY
blocker for a borrowed base (expect exactly its message). (c) Validate the downstream
machinery on the borrowed shape via the `V* vals` laundering probe. (d) Confirm dup-refusal
`return false;` exit-drops a `unique V*` / `unique IShape` argument (gap-1 fix) in a btree-
shaped repro. Abort criteria: if (a) or (c) mis-frees, the gap is bigger than the guard -
re-plan before touching the compiler.

**Stage 0 RESULT (2026-07-23): PASS - clear to proceed to Stage 1.** Probes in scratch/bsr0_*.cb
against the Release binary at 009fcdb, all legs cold AND warm cache, HeapAudit oracle (+ ASan on
the (c) legs, no crash/UAF). (a) local-n and move-param-n slot moves pass for all 8 V shapes,
typed-local recovery passes for string / unique R* / unique IShape. (b) borrowed base fails with
exactly guard #2's message and no other diagnostic. (c) laundering probe passes for string,
unique R*, unique IShape, Holder{unique R*} - but ONLY via the generic-V spelling
(`V* vals = &n->values[0]`); a CONCRETE spelling (`R** vals`) is not ElementOwningUnique-stamped
and SILENTLY LEAKS (bsr0_c_launder_uniqueptr_concrete_FAIL.cb) - the known declarator-star
demotion, but note it compiles without error; possible future diagnostic candidate, out of scope
here. (d) dup-refusal bare `return false;` exit-drops unique R* and unique IShape in both sink
forms. No cold/warm divergence anywhere.

**Stage 1 - compiler change [opus].** Add `&& !argNV.IsElementAccess` to guard #2 in
ParseMoveExpression (MainListener.h ~14632). Pre-flight per the Part 4 discipline: enumerate
err-test/message dependencies (expected: none flip). New err leg: whole-field move through a
borrowed param still rejected; delete-of-recovered-borrow-element rejected. New positive leg
in Test/test_collection_leaks.cb: a generic helper taking a borrowed node-like pointer
releasing `n->values[i]` across every element kind, plus caller-still-holds-n (no sink
inference on the base). Verify: test.sh Release cold+warm zero delta, oracle clean.

**Stage 2 - btree collapse [sonnet, .cb-only].** Per the Section 2 table: delete `_freeValue`,
`_freeKey`, `_clearValue`, `_clearKey`, `_destroyKey`; dup refusals to bare `return false;`;
release ladders to `_ = move` (or a two-line `_releaseValueAt(n, i)`/`_releaseKeyAt(n, i)`
mirroring dictionary's shape); `_splitChild`/`_borrowFrom*`/merge fixups to slot-to-slot
`= move`; `_placeValue` to a direct store with one tailored diagnostic; `_freeSubtree` to
recurse + `delete n` if the destructor-based form probes clean (else the `_ = move` loop).
Keep transfer overloads, key-copy policy, and the concurrent API untouched; update the
ownership comments (the `_freeValue` essay dies with the pattern). `_descendSplitting`'s
"leaf as a genuine LOCAL" motivation becomes obsolete - keep the helper (both insert entry
points share it) but fix the comment.

**Stage 3 - verification bar (every stage, final sweep here).**
- `test.sh Release` cold AND warm (the `--init` leg is mandatory - core .cb changed).
- Example gate: `example_mac.sh` 35/0 (interp.cb and ui examples exercise btree).
- Leak oracle: extend the existing btree matrix in `Test/test_collection_leaks.cb` (~line
  2348) with legs per element kind for: set-overwrite, remove with forced borrow AND merge
  rebalance (build >1 node, remove to underflow both directions), split under owning K
  (string keys - `_dupKey`/separator paths), dup-refusal for every V kind incl. NAMED
  sources, clear()/teardown, all HeapAudit + ASan clean, cold+warm.
- `Test/test_hpc.cb` concurrent legs unchanged (lookup/insert untouched); `test_lsp.sh`.
- err tests: the two borrow-field-move tests + err_btree_noncopyable_value.cb +
  err_moved_out_param_unique_{ptr,iface}.cb keep firing.
- Acceptance: btree.cb's ownership-driven `if const` inventory reduces to the accepted
  residue only (transfer-overload `!is_pointer(V)` guards, key-copy/`_storeKey` policy,
  tailored copy diagnostics, hash-free `_lt/_le/_eq` are not ownership); `_freeValue` and
  every explicit release-by-move-param helper deleted.

## Completion record (2026-07-23)

Stage 1 (1eeea60): guard #2 narrowed exactly as designed. Pre-flight confirmed both err tests
are whole-field shapes (no flips) and zero Test/example uses of the newly-legal spelling.
Positive legs (SlotNode<K,V> helpers, 5 element kinds, slot-to-slot, caller-still-holds-n) and
err legs (element-adjacent whole-field, delete-of-recovered-borrowed-element) added. Review
clean in one round. Review also surfaced a PRE-EXISTING crash, filed:
internal/issue/move-element-of-returned-temporary-crashes.md (`move <call>().arr[i]` segfaults;
whole-field form is cleanly rejected, element form slips the addressability guard).

Stage 2 (629015f): collapsed per the Section 2 table with TWO deviations, both now the
documented shape in btree.cb:

1. `_placeValue` value-type arm KEEPS explicit `.copy()` dispatch. A bare `n->values[i] = value`
   into an INLINE fixed array is a bit-relocate at codegen (the load-bearing invariant), not the
   deep-copying heap-`V*` element store - a bare store aliased the caller's buffer (caught by
   test_hpc "btree<val> set copies (slot)"). Inline-array stores and heap-buffer stores have
   different `=` semantics; Section 2's "Part 6 store semantics" assumption only holds for the
   latter.
2. `_clearKey`/`_clearValue` SURVIVE as minimal abandon-clear helpers for the rebalance
   handoffs. Review round 2 found the planned `= move` conversion of the borrow/merge fixups
   caused a deterministic double-free/UAF of owning keys at depth: `dst = move src` releases
   dst's PRIOR owning content, but at those sites the prior content was an alias created by a
   deliberate bit-relocate one line earlier (Risk (b)/(c) violated by the first attempt). The
   handoffs/down-shifts are back to plain bit-relocate + abandon-clear; `= move` survives only
   at fresh-hole / release-then-fill / unaliased-separator sites (all 20 surviving move sites
   classified safe in review round 3, 600-key random-order stress ASan-clean).

Test surface: test_collection_leaks 509 legs (was 452 pre-plan) incl. 300-key/250-remove
owning-K underflow in both directions and engineered borrow-only value legs (dtor-count
oracle); bar green cold+warm (482/0/8), examples 35/0, test_hpc 259/259, LSP 152/0.
_freeSubtree is the destructor-based form (recurse children + delete n; `next` is a borrow,
not followed). Accepted `if const` residue: `_storeKey`/`_dupKey` key policy, tailored
`!is_copyable` diagnostics, `!is_pointer(V)` transfer-overload guards, the value-type store
arms (deviation 1), and `_lt/_le/_eq` comparators (non-ownership).
