# Field ownership: the `unique` keyword

Created 2026-07-16. **Status: Stages 1-3 DONE (2026-07-16).** Both open decisions are settled
(2026-07-16): the keyword is `unique`, and recursive chains are rejected. See Decisions below.
Nothing is migrated to `unique` yet - Stages 1-3 shipped the mechanism only; Migration is next.

Supersedes nothing. Complements [ownership-move-alias-discipline](ownership-move-alias-discipline.md),
which stays correct for *parameters*; this plan addresses *fields*, which that note does not cover
(and see the Appendix - that note has three factual errors worth fixing).

## Problem

A struct cannot own a raw pointer field. The destructor's field loop
(`LLVMBackend.h:2722-2733`) filters pointers out before it ever asks the ownership question:

```cpp
const auto& f = dsIt->second.StructFields[i];
if (f.Pointer || f.ElemPointer || f.IsArrayView || f.IsSimd || f.IsBitfield || f.IsPadding)
    continue;
if (f.ConstArraySize > 0) continue;
if (llvm::Function* childDtor = GetOrCreateFullDestructor(f.TypeName))
    work.push_back({ i, childDtor });
```

That loop is otherwise exactly C++ memberwise destruction: it asks "does this field's TYPE have a
destructor?" and nothing else (`IsOwningValueType`, `LLVMBackend.h:2846-2853`, is literally
`GetOrCreateFullDestructor(typeName) != nullptr`). It is why `string` fields already auto-destruct.

So today, ownership of a pointer field exists **only** as a hand-written destructor plus a comment.
`DeclTypeAndValue` (`LLVMBackend.h:645-678`) carries no ownership flag at all. Consequences:

- A pointer field with no hand-written dtor leaks, silently, forever.
- `doc/LANGUAGE.md:1075` claims "struct fields own their pointers". The compiler does not implement
  that. The doc describes a convention, not a behavior.
- `ClosureCaptureDeepCopyable` (`LLVMBackend.h:2887-2897`) bails on **any** pointer field, because
  own-vs-borrow is unknowable. It cannot be more precise than that today.

## Design

Add a soft keyword marking a pointer field as owning. The synthesized destructor deletes it.

```cflat
struct Tree
{
    unique Node* _root = nullptr;   // synthesized dtor deletes _root
};
```

Replacing:

```cflat
struct Tree
{
    Node* _root = nullptr;
    ~Tree() { if (_root != nullptr) delete _root; }
};
```

**The keyword is opt-in and additive.** An unmarked raw `T*` field behaves exactly as it does today
(never auto-freed). This is the property that makes the change affordable - every hard case opts out
by doing nothing:

| Hard case | Resolution |
|---|---|
| `json.cb` / `xml.cb` arena-vs-heap duality (`firstChild` owns on the heap path, borrows on the arena path - `json.cb:390-394`, `:405`) | does not use `unique`; keeps its hand-written dtor. Unchanged. |
| `ComPtr<T>` (`com.cb:129`) - owns, but releases via `lpVtbl->Release`, not `delete` | does not use `unique`. Unchanged. |
| ~76 OS-handle fields (`File._handle`, `Socket._handle`, ...) - freed by `CloseHandle`/`munmap` | does not use `unique`. Unchanged. |
| ~46 C-ABI layout mirrors (`ui_native/win32.cb`, `process.cb:60-96`) | must stay raw for layout. Unchanged. |
| Container element ownership (`list<T*>` owns its elements - decided, shipped, `.dtordeferred`) | untouched. `unique` is field-only and does not redefine raw `T*`. |

The last row is the decisive one. It is why this beats the two alternatives considered:

- **Rejected: `alias` field qualifier with owning-by-default.** Taxes the common case (~84 borrow
  fields would need annotating), and "owning by default" would be a claim the compiler does not
  back with a free. Also cannot express the arena duality.
- **Rejected: `box<T>` owning pointer type (the C++/Rust shape).** Cleaner in principle - ownership
  in the type, composes into generics - but redefines raw `T*` as "borrow" globally, which directly
  contradicts the shipped container owned-model decision, and drags in a `new`-produces-a-box story
  plus `operator->` ergonomics. Not worth it for a destruction fix.

### Precedent

`gsl::owner<T*>` (C++ Core Guidelines) is exactly this marker on a raw pointer, invented because
`unique_ptr` cannot be retrofitted into C-ABI and legacy layouts. Objective-C ARC's
`__strong`/`__weak` are the same idea as declaration qualifiers. Both are *inert* - they only feed
static analyzers. `unique` goes further by synthesizing the destruction.

### Why the plumbing is cheap

1. **No grammar change.** `move` / `alias` / `bond` already parse on fields as soft keywords and
   land in `StructFields[i].IsMove` / `.IsAlias` / `.IsBond` - inert today, but the route is proven.
   Text-match in both `ParseDeclarationSpecifiers` copies (`MainListener.h:739` ForwardRefScanner,
   `:2347` codegen; `alias` at `:744`/`:2352`), per the CLAUDE.md soft-keyword rule. Do NOT add to
   the ANTLR lexer.
2. ~~**Per-field properties already flow to the access site.** `AllocAlignValue` is copied onto the
   field-access `NamedVariable`; `unique` follows the same route.~~ **WRONG - no route needed
   (verified 2026-07-16).** `IsUnique` lives on `TypeAndValue`, and `MainListener.h:13290`/`:13434`
   bind `fieldType = StructFields[fieldIndex]` then `namedVar.TypeAndValue = fieldType`, so the
   flag is already present at the assignment site. The `AllocAlignValue` route exists because that
   value is copied onto a *separate* `NamedVariable::AllocAlignment` member. A budgeted step that
   turned out to be a no-op.
3. **Composes with `alignas(slot, alloc)`.** Both are field qualifiers; the synthesized dtor needs
   `AllocAlignValue` to free an over-aligned allocation correctly.

### LOAD-BEARING: the `--init` serializer

`unique` adds a field an analysis reads. It **MUST** be added to the cache round-trip in the same
change, or it is silently dropped on a warm cache and the `expect_error` tests stop firing. See
`internal/testing-notes.md`.

DONE (Stage 1): `IsUnique` sits on `TypeAndValue` next to `IsMove`/`IsBond` (NOT on
`DeclTypeAndValue` as first written - `DeclTypeAndValue` derives from it, and `SerializeDtav`
delegates to `SerializeTav`, so one line each way covers struct fields too). Key `"uq"`, following
`IsMove`'s `"mv"`, in `SerializeTav`/`DeserializeTav` (`LLVMBackend.cpp:3773`/`:3826`).

Corrections to this section as originally written: **there is no serializer in `LLVMBackend.h`** -
the cited `:15577`/`:15607` do not exist and never did (unverified line refs carried in from an
earlier session). The round-trip lives only in `LLVMBackend.cpp`. Note also that the round-trip is
still **inert**: nothing in `core/` uses `unique` until Migration, so a green warm-cache run is not
evidence it works.

## Decisions (settled 2026-07-16)

### D1 - keyword name. SETTLED: `unique`.

| Candidate | For | Against |
|---|---|---|
| **`unique`** | pairs with `alias` (already means "you do not own this, do not free it") on the same axis, opposite pole; short, like `move`/`bond` | cflat uses alias/noalias vocabulary for the OPTIMIZER (`span<T>` noalias vs `view<T>` may-alias, `NoaliasScopeId`). Could be misread as a `restrict`-style promise. Judged mild: `alias` is already an ownership word in cflat, not an optimizer word |
| `owner` | `gsl::owner` pedigree | **collides in-tree**: `_BA_PageHdr._owner` / `_BA_LargeHdr._owner` (`bucket_allocator.cb:48`, `:76`) are back-pointers meaning "the allocator that owns ME" - the opposite direction. `owner BucketAllocator* _owner;` reads as a contradiction |
| `owned` | dodges both collisions | adjective attaches to the wrong noun (the field is not owned; the pointee is) |

### D2 - recursion. SETTLED: reject. A recursive structure owns its own teardown.

**Rule: a recursive data structure cannot use `unique`. Its author designs the teardown.**
Confirmed 2026-07-16 after a full re-examination (below); the Stage 1 implementation already does
this and needs no change.

Error when a `unique` field's pointee type transitively reaches a `unique` field of the same type.
Transitive, not a `field type == my type` name test: `A` owning a `unique B*` while `B` owns a
`unique A*` is the same cycle one hop longer.

**Why: there is no good answer in the general case.** Both mechanisms the compiler could synthesize
are defective, in ways that depend on data the compiler cannot see:

- **Recursive teardown** (the natural synthesis) costs one stack frame per link on a chain, and one
  per level on a tree. A balanced tree is ~5-20 frames and fine; a sorted-insert BST or a long
  `next` chain is N frames and overflows during teardown, far from the code that built it. C++ has
  exactly this bug with `unique_ptr` and has never fixed it.
- **Iterative teardown** (worklist instead of self-call) is correct at any depth, but needs a heap
  worklist allocated *during destruction*, which can itself fail under memory pressure.

The shape that is safe is a property of the data, not the type - and the author is the only one who
knows it. So the author writes the destructor. `unique` covers what the compiler can handle
unambiguously; recursion is not that.

The chain-vs-tree distinction is what makes this non-obvious and is worth recording: the original
argument for rejecting used only `unique JsonNode* next` (a chain, N frames), which made rejection
look free. It is not free - it also rejects trees (`btree_node.children[17]`, `btree.cb:77`, whose
pointee is `btree_node<K,V>` itself), which recurse only per level. Rejection was briefly reversed
on that basis and then re-confirmed: allowing trees means allowing degenerate trees, and there is no
sound static test separating them. Heuristics considered and rejected: "reject 1 self-pointer, allow
2+" (a skewed binary tree is linear too; a 3-element chain is harmless).

**Consequences, accepted:**

- `btree_node` keeps its hand-written destructor. Stage 3's fixed-array bullet loses its motivating
  case (see Stage 3).
- `json.cb:208` / `xml.cb:211` keep theirs - they opt out over the arena/heap duality anyway.
- cflat still accepts a hand-written `~JsonNode() { delete next; }`, which recurses identically. We
  decline to *synthesize* a footgun the author may still write deliberately, knowing their data. The
  line is that synthesized code should never need auditing.

## Stages

### Stage 1 - parse and destruct - DONE 2026-07-16

- DONE. Text-match in both `ParseDeclarationSpecifiers` copies; `TypeAndValue::IsUnique` (which
  `DeclTypeAndValue` inherits, so it reaches `StructFields` with no extra plumbing).
- DONE. Serializer round-trip (`"uq"`) in `SerializeTav` / `DeserializeTav`. Currently INERT - no
  core type uses `unique` yet - but it must land before the first migration or a warm `--init`
  cache silently drops the flag.
- DONE. `GetOrCreateFullDestructor` collects `unique` pointer fields ahead of the pointer skip and
  emits a null-checked delete via `EmitUniqueFieldDelete`, honoring `AllocAlignValue`
  (`__delete_aligned` when over-aligned). The pointee dtor binds through
  `GetFullDestructorForDelete`, so an incomplete pointee is not baked out as null. A `unique`
  field is pushed even when the pointee is trivially destructible - the block still needs freeing.
- DONE. Position is enforced with a scoped flag: `ParseDeclarationList` arms `inStructFieldDecl_`
  around its specifier parse, so `unique` on a local / parameter / return type is rejected by
  fall-through rather than by enumerating every non-field site.
- DONE. Rejections: non-pointer, `T**`, fixed array, array-view, simd, bitfield, non-field
  position, and the D2 cycle. All `LogErrorContext` (or `LogError` for the cycle walk).
- DONE. D2 uses a registration-time transitive walk (`RejectUniqueDestructionCycles` at
  `CreateStructType`), not the `fullDestructorInProgress_` fallback: it fires regardless of
  whether the type is ever destructed. It is transitive (self, mutual, and N-hop), and reports
  from whichever member is registered last - the one that closes the chain.
- Tests: `Test/errors/err_unique_*.cb` (8), positive cases in `Test/test_move.cb`
  (`testUniqueField`, dtor-count oracle) and `Test/test_collection_leaks.cb` (HeapAudit oracle).

### Stage 2 - assignment discipline - DONE 2026-07-16

- DONE. **Reassignment destructs first.** The assignment site calls the same
  `EmitUniqueFieldDelete` the synthesized destructor uses, so a field is freed identically at
  teardown and at reassignment. `nullptr` frees the old pointee (the sanctioned release-early
  idiom); `new T()` frees and re-roots. Honors `AllocAlignValue` (`__delete_aligned`) via the
  shared helper. Proven by the HeapAudit oracle: 100 reassignments leak 0, while the same
  loop on an unmarked pointer field leaks 10/10.
- DONE. **Self-assign is guarded at RUNTIME**, not by name. `EmitUniqueFieldDelete` gained an
  optional `replacement` argument (the pointer about to be stored) and skips the free when the
  field already holds it. Name matching is not sufficient: a bare `item = item` inside a method
  resolves through `GetMemberVariable`, which deliberately leaves `FieldName`/`OwningStructName`
  empty (`MainListener.h:17538`) so the delete-encapsulation rule does not fire on self-access -
  so `sameField` cannot see it. SSA comparison fails too (each access re-loads `this`). The
  runtime compare also covers a source that merely aliases the field.
- DONE. **Storing a borrow into a `unique` field is an error** (Trap A), via `rightNV.IsBorrowed`
  next to the sibling rejections. Two messages: the borrowed-parameter case, and the case where
  the source is a field reached through a borrowed parameter.
- DONE. **A struct with a `unique` field is not memberwise-copyable.** The real gap was NOT
  `Holder b = a;` (that already auto-MOVES, with use-after-move enforced) nor a field-to-field
  copy (already rejected via `IsOwningValueType`, which is true once the type has a synthesized
  dtor). It was the synthesized `.copy()`, whose skip-list shallow-copies pointer fields -
  ASAN-confirmed double-free, transitively through a by-value member too. Closed with
  `TypeOwnsUniquePointer` (transitive), which bails `GetOrCreateMemberwiseCopy` and reports at
  the copy-synthesis choke point.
- DONE. **`delete` on a `unique` field is rejected** - a double-free Stage 1 opened, since the
  synthesized dtor runs the user dtor first and then deletes the field. Keyed on `IsUnique`
  rather than `FieldName` so the bare self-field form (the one that matters) is caught.
- Tests: `Test/errors/err_unique_borrow_into_field.cb`, `err_unique_delete_field.cb`,
  `err_unique_memberwise_copy.cb`; positives in `Test/test_move.cb` (`testUniqueField`, 6 -> 17
  assertions) and `Test/test_collection_leaks.cb` (reassignment leak oracle).

**Uninitialized-slot hazard: investigated, slot is trusted.** Every construction path
zero-initializes: `new T()`, `new T[n]`, stack locals, and user-ctor entry all run a synthesized
default ctor that returns `zeroinitializer` (verified in IR and by poisoning + forcing heap
reuse). The only hole is `malloc` + cast, which bypasses construction entirely - and that ALREADY
crashes Stage 1's synthesized destructor on the same slot (verified), and breaks a `string` field
identically. It is out of contract, not a `unique`-specific hazard. Requiring an initializer
(`unique Node* p = nullptr;`) was considered and REJECTED: `malloc` bypasses field initializers
too, so it would not close the hole - pure ceremony.

**CORRECTION 2026-07-16: "every construction path" was overstated - a FIXED ARRAY OF STRUCTS is not
reliably zero-initialized.** See
[fixed-array-of-structs-not-default-initialized](../issue/fixed-array-of-structs-not-default-initialized.md).
The paths actually verified above (scalar stack local, `new T()`, `new T[n]`, user-ctor entry) all
hold and were tested; **array-of-struct elements were never tested and are never initialized**.
`D_string[4]` yields a garbage `string` header whose destructor reads the owned bit out of garbage
`_len` - a latent arbitrary-free. `Box[4]` (a `unique` scalar field) happened to zero-init in
testing; per the pinned rule that is stack luck, not safety, so **do not read it as clean**. A
garbage `unique` pointer would be freed by the synthesized destructor at scope exit. Not a
`unique`-caused bug - a pre-existing initialization gap this feature would ride into.

(An earlier version of this note claimed the behaviour varied by element type - hand-written dtor
zero-inits, synthesized does so for `unique` but not `string`. **That was noise**: it was reading
whatever stack garbage was present, and the results inverted between runs. The rule is simply
"arrays are never initialized, scalars always are". Recorded because the false per-type table is
exactly the kind of detail that survives into someone's fix attempt.)

### Stage 3 - arrays and precision. CANCELLED 2026-07-16: all three items are dead.

Every item was verified before building, per the caveat this section used to carry. All three
failed. Stage 3 as designed does not exist; the only surviving work is Trap B, below.

**The caveat was right, and is worth keeping in mind for Migration.** Stage 1 (destruction) matched
its design almost exactly; Stage 2 (assignment) had three of four claims fail on contact; Stage 3
(precision) was 0 for 3. The gradient tracks provenance exactly: Stage 1 was written from code that
had been exercised, Stages 2-3 from code that had only been read.

- ~~`unique Node* children[17];` - `btree_node` (`btree.cb:77`) needs the dtor to loop.~~
  **DEAD: no motivating case exists.** `btree_node.children[17]` is the ONLY fixed array of pointer
  fields in the entire tree (verified by sweep over `core/` and `example/`), and it is recursive -
  the pointee is `btree_node<K,V>` itself - so under D2 it can never use `unique` regardless. Making
  the destructor loop over fixed arrays would ship a feature with zero users. The Stage 1 rejection
  (`'unique' on field 'X.y': fixed arrays are not supported yet...`) should stay; consider dropping
  "yet" from the message.
- ~~Sharpen `ClosureCaptureDeepCopyable` to bail only on `unique` fields instead of on every pointer
  field.~~ **DEAD: would introduce a double-free.** The bullet silently assumed an unmarked `T*`
  field is a borrow - which is the `box<T>` premise this plan explicitly REJECTED (see Design). It
  is not true: the ~34 verified-owning unmarked pointer fields (`json.cb` `firstChild`, `ComPtr`,
  ...) own via a hand-written dtor. Sharpening would declare those types deep-copyable, flipping
  their closure capture from by-reference to by-VALUE, shallow-copying the owned pointer into the
  env, and double-freeing at teardown. The bail is load-bearing, not conservatism to be tuned away.

  **CORRECTION 2026-07-16 - the evidence originally cited here was FALSE, and I wrote it.** This
  bullet claimed: *"Baseline confirmed by spike: a hand-owned struct captured in a closure today
  frees exactly once, precisely BECAUSE the blunt pointer bail forces by-reference capture."* That
  spike was confounded. Its destructor nulled the pointer after deleting (`{ delete p; p = nullptr; }`),
  which masked the second free. Isolated A/B with the nulling as the only variable:

  | dtor | result |
  |---|---|
  | `{ delete p; p = nullptr; }` | freed once, clean |
  | `{ delete p; }` (the `json.cb` / `ComPtr` shape) | **ASAN double-free** |

  By-reference capture happens, but it does NOT prevent double *destruction*: the closure env
  destroys the referenced object, and it is destroyed again at its own scope exit. The conclusion
  (do not sharpen) still stands - by-VALUE would shallow-copy the pointer and double-free too, so
  both settings are wrong - but the bail is not the thing keeping the baseline safe, because the
  baseline is NOT safe. See the closure double-free issue below.
- ~~Same for the memberwise-copy synth skip-list: sharpen so a type owning a `unique` pointer can
  have a synthesized copy that deep-clones the pointee.~~ **DEAD: the blunt behavior IS the correct
  semantic.** `std::unique_ptr` is deliberately non-copyable and move-only; a struct holding one is
  too. Stage 2's outcome - no synthesized copy, plus an error naming the fix ("write a `copy()` that
  clones the pointee, or `move` instead of copying") - is precisely that contract. Auto-deep-cloning
  would DIVERGE from the model `unique` is named after, silently turning an intended move into a
  deep copy. Nothing to sharpen.
**The one surviving item, which became Stage 3 in its entirety:**

### Stage 3 - Trap B through a `unique` field - DONE 2026-07-16

Laundering a `unique` field through a plain local defeated every Stage 2 check and double-freed
(ASAN-confirmed). NOT a regression - the hand-written `~Box() { delete p; }` equivalent has always
had it (re-confirmed by ASAN this session, and deliberately still uncaught) - but `unique` made it
reachable with less code, and the fix the pre-`unique` world did not have is cheap.

- DONE. **A local bound from a `unique` field inherits the borrow.** One new source-classification
  clause at the `ParseDeclaration` init site, next to the existing `IsBorrowed` propagation, plus
  `NamedVariable::BorrowedUniqueField` ("Struct.field") so the diagnostics can name the real owner
  instead of a nonexistent "borrowed parameter". Reads/writes through the alias stay legal; only
  the free is rejected. Cost: the flag rides `NamedVariable`, which is **not** serialized, so no
  `--init` round-trip change (contrast Stage 1's `TypeAndValue::IsUnique`, which needed one).
- DONE. **Both spellings are closed.** The plan text above framed Trap B as `delete alias`, but
  the canonical Trap B in [ownership-move-alias-discipline](ownership-move-alias-discipline.md)
  is `consume(move alias)` - and fixing only `delete` left it open (ASAN-confirmed double-free)
  while the new delete diagnostic actively steers users toward `move`. `move` on a
  unique-field-derived local is now rejected in `ParseMoveExpression`. Forwarding an ORDINARY
  borrow as `move` stays legal, as the discipline note requires: that rule rests on the programmer
  asserting the borrow is dead, which for a `unique` field can never hold - the field's destructor
  is synthesized and *will* run. Narrowly keyed on `BorrowedUniqueField`, so ordinary forwarding
  is untouched.
- DONE. `move b.p` remains the sanctioned extraction, unregressed: it returns a fresh
  NamedVariable with no `Storage` and sets `lastOwningResult`, so it is not a field read and never
  reaches the new clause. Covered by `test_move.cb` (dtor-count oracle) and the HeapAudit oracle.
- **No regression on the ~254 ordinary pointer fields.** The new clause is keyed on
  `TypeAndValue.IsUnique`, which only ever lands on a field; unmarked `T*` laundering behaves
  exactly as before (pinned by a `plain_field_alias_delete_still_legal` case). `example.bat`
  90/0/27 is the evidence that mattered.
- Tests: `Test/errors/err_unique_delete_alias.cb`, `err_unique_move_alias.cb`,
  `err_unique_alias_into_field.cb`; positives in `Test/test_move.cb` (`testUniqueField`,
  17 -> 26 assertions) and the `test_collection_leaks.cb` HeapAudit oracle (105 -> 107).

**Found, NOT fixed - a direct field-to-field copy is still unguarded.** `c.p = a.p;` between two
`unique` fields compiles clean and double-frees (both synthesized destructors free one pointee).
The Stage 2 reject at the store site keys on `rightNV.IsBorrowed`, which is false for a field read
off a plain local, and Stage 2's field-to-field rule keys on `IsOwningValueType`, which is false
for a raw pointer. Deliberately out of Stage 3's scope: the laundered form (`Res* alias = a.p;
c.p = alias;`) IS now rejected, so this is the last hole of the family, but closing it means a new
rule at the store site rather than a reuse of the borrow mechanism.

**Treat this as a MIGRATION BLOCKER, not a known gap** (assessed 2026-07-16, ASAN-confirmed by
spike). The asymmetry is actively harmful, not merely incomplete: the LONGER, more obviously
suspicious spelling now errors, while the SHORTER, more natural one compiles clean and
double-frees. A user who hits the Stage 3 laundering error is steered by it toward `move a.p`
(correct) - but the equally obvious "fix" of collapsing it to `c.p = a.p` silently produces the
heap bug the error existed to prevent. Shipping the diagnostic without this closes worse than
shipping neither. Close it before migrating the 34 fields, or the first migration lands a footgun
the compiler half-warns about.

Shape of the fix: a new rule at the store site keyed on `namedVar.TypeAndValue.IsUnique &&
rightNV.TypeAndValue.IsUnique` (both sides are `unique` fields) - the RHS state is already there,
since `namedVar.TypeAndValue = fieldType` carries `IsUnique` onto a field read. `move a.p` must
stay legal (it nulls the source). Verify against the Stage 2 sibling rejections before writing it.

### Stage 4 - migrate `list` / `dictionary` to `unique`. REJECTED 2026-07-16: not buildable.

Proposed and dropped the same day, after checking the code rather than assuming. Recorded because
"migrate the containers" is an obvious thing to re-propose, and the answer is not obvious without
reading `~list` / `~dictionary`.

**Every pointer field in both types is an ARRAY, not a single object.** `list<T>._data` is
`new T[newCap]`, freed by `delete[_size] _data`; `dictionary._keys` / `._values` / `._status` are
`new K[cap]` / `new V[cap]` / `new char[cap]`. `unique` emits a SINGLE-object delete (pointee dtor
once, then `operator delete`), so pointing it at `_data` would destruct element 0, leak elements
1..N-1, and free an array allocation through the scalar path.

The destructors need three things `unique` structurally cannot carry:

- a **runtime element count** (`_size`, `_capacity`) - `delete[_size] _data`
- a **compile-time type branch** - `if const (is_pointer(T))` selects element-delete vs value-dtor
- **array-delete** semantics

`~dictionary()` additionally does a **sparse** walk (`if (_status[i] == 1)`) with per-slot branches
on `is_pointer(V)` and `is_string(K)`. There is no way to spell "destruct only the occupied slots
of a hash table" as a field qualifier.

It also collides with the Design table above: "Container element ownership - untouched. `unique` is
field-only and does not redefine raw `T*`." That row is *why* `unique` beat `box<T>`; migrating the
containers reopens exactly what made the design affordable.

What it would actually take is a different feature - a counted-array qualifier naming the count
field (`unique(_size) T* _data;`) plus adopting the container element-ownership model. New qualifier
form, a field cross-reference, element semantics: real machinery to delete ~10 lines from `list.cb`
and ~18 from `dictionary.cb`, at two call sites. `delete[_size]` already expresses the counted-array
free perfectly well. If revived, spec it as its own design - do not bolt it onto this plan.

## Code review 2026-07-16 - findings

Two adversarial reviews (memory-safety lens, integration lens) plus a maintainer pass over the full
388-line feature diff. **No regression to ordinary code** - that was the priority target and it
holds: zero `uq.*` markers in non-`unique` IR, the dtor collect loop is byte-identical for the 254
ordinary pointer fields, `ClosureCaptureDeepCopyable` untouched, serializer round-trip correct,
soft keyword collides with no identifier. Every finding below is a hole in the feature's OWN rules.

### Remediation status (2026-07-16)

Findings 1, 3, 4 and 5 are **FIXED** (see "Stage 4 - code-review remediation" and "Stage 5 -
interface field contract" below). Only finding 2 (the `(T*)` cast hole) remains open; it is
recorded and untouched. Migration stays BLOCKED on finding 2 alone.

### Blocks migration - all CONFIRMED with ASAN repros

1. **Interface field slot: both guards absent, and no correct spelling exists.** A `unique` field
   reached through an interface's byte-offset slot gets neither the reassign-free (old pointee
   LEAKS) nor the Trap A borrow check (field frees the caller's pointer -> DOUBLE-FREE). Root cause:
   `MainListener.h:13355`/`:13364` bind `namedVar.TypeAndValue` from the INTERFACE's field decl, so
   `IsUnique` is false. Note `destIsStructField` (`:7963`) deliberately ORs in `IsInterfaceField`
   "reached via the interface's byte-offset slot" - the path was routed INTO the guards on purpose,
   but the flag never arrives. And the field cannot be marked `unique` on the interface:
   `ParseInterfaceFields` (`:2933`) does not arm `StructFieldDeclGuard`, so the position check
   rejects it. Both spellings are broken. **Worst finding: this is idiomatic code.**

   **RESOLVED 2026-07-16 - `unique` joins the interface field contract.** Not fixable by propagating
   the implementor's flag: through dynamic dispatch the concrete type is unknown, and two
   implementors could disagree. Ownership either lives in the CONTRACT or is not expressible across
   an interface at all.

   The deciding evidence: **the interface contract already carries ownership, and it works.**
   `core/interfaces.cb` ships `IChannel.push(move T item)` ("ownership transferred for pointer
   types") and `ICopyable.copy()` returning `move TSelf`. Verified by spike that `move` transfers
   correctly through dynamic dispatch (ASAN-clean, freed exactly once). So `unique Res* item;` on an
   interface is the field analogue of `move T item` on a method parameter. Rejecting it would make
   `unique` the ONE ownership qualifier that cannot cross an interface boundary - arbitrary and
   unexplainable.

   Rejected alternative: **reject exposing a `unique` field through an interface field slot.**
   Argued for on reversibility (error -> working code later breaks nobody) and on there being zero
   pointer-typed interface fields in the tree today (69 interface fields, all value types), so it
   would cost nothing. Dropped because the "speculative generality" premise was false - ownership in
   the interface contract is established, shipped design, not speculation.

   The fix is three changes riding existing machinery:
   1. Arm `StructFieldDeclGuard` in `ParseInterfaceFields` (`MainListener.h:2933`) - one line; makes
      `unique` spellable on an interface field.
   2. Add `impl->IsUnique != f.IsUnique` to the EXISTING contract check at `LLVMBackend.h:7551`,
      which already compares `TypeName` / `Pointer` / `IsInterface` and reports "class 'X' field 'y'
      has type ... but interface 'I' declares it as ...".
   3. Nothing else - `interfaceFields` is `std::vector<TypeAndValue>`, which already carries
      `IsUnique`, so the access site (`:13355`/`:13364`) picks it up and both guards fire with zero
      plumbing. That is the same reason `destIsStructField` was already routed to include
      `IsInterfaceField`.

   Consequence, accepted: an interface that exposes `Res* item` WITHOUT `unique` forbids every
   implementor from marking it `unique`. That uniformity is not incidental - implementor-owns +
   interface-silent IS this bug. It matches what `push(move T item)` already imposes.

   Note this makes interface FIELDS better-enforced than interface METHODS, whose contract check
   compares only `TypeName` and ignores `IsMove` entirely - a separate pre-existing bug, see
   [interface-method-ownership-contract-unchecked](../issue/interface-method-ownership-contract-unchecked.md).
   That is the right direction, not an inconsistency to level down.
2. **A `(T*)` cast defeats all three Stage 3 rules.** STILL OPEN - moved to
   [cast-defeats-unique-borrow-tracking](../issue/cast-defeats-unique-borrow-tracking.md) 2026-07-16,
   because **the correct behavior is undecided** (should a cast preserve borrow provenance, be an
   explicit escape hatch, or be rejected on a `unique` field?) and that decision is the maintainer's,
   not a remediation task. `Res* a = (Res*)b.p; delete a;` compiles and double-frees while the
   un-cast form is rejected; same for `move` and store-into-field. Worse than the field-to-field
   blocker was: the escaping spelling is the identical code PLUS a cast - exactly what a user
   reaches for when the compiler rejects a pointer assignment - and the `delete` diagnostic steers
   toward `move`, which the same cast also bypasses. **This is the sole remaining migration
   blocker.**
3. **Brace-init / element sugar bypasses Trap A.** `Box c = { item = borrowed };` compiles and
   double-frees; the identical `=` spelling is correctly rejected. `EmitOneFieldInit`
   (`MainListener.h:10881`) is a SECOND independent field-store path - it handles `string` deep-copy,
   interface boxing and closure envs, and has no `unique` handling. Reached from brace-init
   (`:11053`) and `<Tag attr=...>` sugar (`:17287`). Reassign-free is correctly not needed (fresh
   slot); Trap A only.
4. **Direct field-to-field `c.p = a.p`** - see the Stage 3 section. Unchanged.
5. **`union` bypasses D2 AND unique-in-union is an arbitrary free.**
   `RejectUniqueDestructionCycles` is wired only into `CreateStructType`, not `CreateUnionType`
   (`LLVMBackend.h:11189`), so `union UBad { unique UBad* p; long long filler; };` compiles and
   emits the unbounded-recursive destructor D2 exists to reject. Separately, a `unique` member of a
   union is freed regardless of the active member: `free(0x4141414141414141)`. Unions destructing
   inactive members is PRE-EXISTING (`union { list<int>; i64 raw; }` crashes identically; `string`
   survives only via its runtime owned bit), but `unique` escalates a crash to an attacker-controlled
   free. `ValidateUniqueField` (`MainListener.h:6119`) should reject a union member - one line,
   consistent with the rejections already there.

### Pre-existing, NOT caused by `unique` - moved to `internal/issue/`

The review surfaced five bugs that predate `unique` and are not blocked on it. Each now has its own
issue file with a verified repro, root cause and fix direction; they are listed here only for the
interaction, which is the part a `unique` reader needs.

| Issue | Interaction with `unique` |
|---|---|
| ~~closure-capture-owning-struct-double-free~~ | **FIXED 2026-07-16, issue file deleted.** Was the worst of the five for this feature's pitch: a `unique`-field struct captured in a closure double-freed, and the only workaround was writing a destructor to make `unique` safe. Root cause was NOT the env teardown (the issue file's guess, wrong - the env is uninvolved, and a by-reference capture owns nothing): the LAMBDA INVOKER BODY registered the capture as an ordinary named local without `IsAliasBorrow`, so `EmitDestructorsForScope` destructed the caller's struct through the borrowed pointer **on every call**. Fix: one line, `captureNV.IsAliasBorrow = true` (`MainListener.h:17033`). Verified: 3 calls -> 0 in-body dtor runs, 1 at the owner's scope exit. |
| ~~interface-method-ownership-contract-unchecked~~ | **FIXED 2026-07-16, issue file deleted.** Stage 5 made interface FIELDS enforce ownership agreement; methods did not, so `VerifyInterfaceMethodContract` (`LLVMBackend.h`) now checks param `IsMove` plus return `IsMove`/`IsAlias`/`IsBond` on the selected overload. It also forced a real `core/interfaces.cb` correction (`IEnumerable`/`IList` were declaring plain returns for what `list<T>` actually hands back as `alias` borrows, and plain params for what it takes by `move`) and exposed that **`IsAlias` was never in the `--init` round-trip** - now `"al"`, alongside `"mv"`/`"bd"`/`"uq"`. |
| ~~alloc-align-clause-indirect-store-unchecked~~ | **FIXED 2026-07-16, issue file deleted.** An indirect store of a plain-`new` block into an `alignas(0,N)` field now errors instead of routing `__delete_aligned` at it. The naive rule ("require agreement whenever the field has a clause") OVER-rejects: type-carried alignment (`struct alignas(64) T`) yields `AllocAlignment == 0` and so "disagrees" with a `64` clause while being perfectly correct - `ElementTypeIsOverAligned` carves it out, mirroring the free site's own recovery. Also note the pre-existing message was unsatisfiable for scalars: the grammar only hangs `alignmentSpecifier` off the ARRAY form of `new`, so there is no `new T alignas(...)` to follow the old advice with. |
| [deref-assign-owning-struct-double-free](../issue/deref-assign-owning-struct-double-free.md) | `*pc = *pa` is a shorter whole-struct spelling of the field-to-field family. `c = a;` correctly auto-moves; the deref lvalue is not a move site. A plain `string` field reproduces it. |
| [container-copy-moves-or-shares-elements](../issue/container-copy-moves-or-shares-elements.md) | **Do not read a green `list<Box>.copy()` as evidence the `unique` copy guard held** - it passes only because the source is silently emptied, so no second owner exists. Once container copy routes through copy resolution it will correctly report the `unique` copy error. Wider than first thought: `list<T*>` nulls source elements too (the doc comment says the opposite), and `dictionary<K,V>.copy()` CRASHES with heap corruption. Blocked on a compiler decision - see the issue. |

### Non-blocking cleanups

- **Dead code**: `GetOrCreateMemberwiseCopy`'s internal `TypeOwnsUniquePointer` bail
  (`LLVMBackend.h:3034-3036`) is unreachable - its only caller (`:13480`) sits in the `else` of the
  same predicate (`:13466`). Not defensible as defence-in-depth: it returns `nullptr` silently and
  the caller discards it, so if it ever fired the user would get `unknown function 'copy'` instead
  of the good message. Remove it or give it a `LogError`. (`return nullptr;` at `:13477` is also
  dead - `LogError` is `[[noreturn]]`.)
- **Trap A prints `unique field ''`** for a bare self-field access in a method/constructor -
  `GetMemberVariable` leaves both `OwningStructName` and `FieldName` empty. The sibling diagnostics
  handle this via `DescribeUniqueFieldOwner` / `SplitEnclosingStruct` (`MainListener.h:11973-11990`);
  the Trap A one at `:8130-8132` was not given them. The constructor case is where users will meet it.
- **`unique int x[4]` reports a pointer message** - the fixed-array check precedes the is-pointer
  check in `ValidateUniqueField`, so a non-pointer array is told the dtor "deletes a single pointer
  and would leak the rest". Order `!f.Pointer` first, or add a pointer precondition.
- `test_move.cb`: `unique_composes_with_user_dtor` and `unique_user_dtor_release_frees_once` are
  byte-identical. One is redundant.
- `test_move.cb`: stale comment says the user dtor "frees and nulls" - it assigns null, and the
  Stage 2 reassignment path does the freeing.
- `TypeOwnsUniquePointer` uses `std::set`, `UniqueChainReaches` uses `std::unordered_set` for the
  same job. Pick one.
- **On the `UniqueFreesItself` rewrite** (Stage 2 changed `delete item; item = nullptr;` to
  `item = nullptr;`): it DOES weaken the test, unavoidably. The original proved a hand-written free
  composes with the synthesized dtor; now both the release and the teardown are the same compiler
  machinery, so it cannot fail the way the original could. Not fixable - Stage 2 made hand-freeing a
  `unique` field illegal, so the original scenario is unrepresentable by design.

### Stage 4 - code-review remediation - DONE 2026-07-16

Findings 3, 4 and 5 only. Findings 1 (interface field slot) and 2 (`(T*)` cast) were deliberately
left alone, as were the non-blocking cleanups.

- DONE (finding 3). **Brace-init / element sugar no longer bypasses the store-site rules.**
  `EmitOneFieldInit` is a second, independent field-store path; it now applies the same two source
  rejections the `=` path does. Both are shared as `RejectBorrowIntoUniqueField` (Trap A) and
  `RejectUniqueFieldToUniqueField`, so the two paths cannot drift in wording or in rule. No
  reassign-free was added: both callers construct a fresh slot, so there is no old pointee.
  Note the plan said "Trap A only" - that was not enough. `rv_brace3.cb` reaches the same path with
  a field-to-field source (`Box c = { item = a.item };`), which Trap A cannot see because a field
  read off a plain local is not a borrow. Fixing brace-init therefore needed finding 4's rule too.
- DONE (finding 4). **Direct field-to-field copy rejected.** New rule at the store site keyed on
  `namedVar.TypeAndValue.IsUnique && IsUniqueFieldRead(rightNV)`. `IsUniqueFieldRead` requires the
  source's `Storage` to be a 2-index struct GEP, which is what keeps `move a.p` legal (it returns a
  fresh NamedVariable with no `Storage`) and keeps plain locals out (an `auto` local bound from a
  field is not a field slot). Ordered AFTER Trap A, so a source reached through a borrowed parameter
  still reports the more precise borrow reason.
  Self-assign needed one addition the plan flagged: `selfFieldAssign` cannot see a bare
  `item = item` inside a method (`GetMemberVariable` leaves `FieldName` empty by design), so
  `selfUniqueFieldAssign` also compares the declared field name when both sides are bare. The
  runtime `replacement` guard in `EmitUniqueFieldDelete` is unchanged and still does the real work.
- DONE (finding 5). **`unique` on a union member is rejected** in `ValidateUniqueField`, ahead of
  the field-shape checks (no union member can ever be `unique`, so a shape complaint would only
  steer the user to fix the shape and land back here). Armed by `UnionFieldDeclGuard`, which carries
  the body's own kind and restores on exit - a struct nested in a union keeps `unique`, and a union
  nested in a struct still rejects it.
  **This subsumes the `CreateUnionType` D2 gap, verified rather than assumed**: `UniqueChainReaches`
  traverses only `IsUnique` pointer fields, so with no union member able to carry `IsUnique` the
  check would be a provable no-op there. Every union built from source flows through
  `ParseDeclarationList` -> `ValidateUniqueField`; the only other `CreateUnionType` caller
  (`LLVMBackend.h:4963`, C-interop anonymous records) builds fields from clang's AST and never sets
  `IsUnique`. `RejectUniqueDestructionCycles` was NOT wired into `CreateUnionType`.
- Tests: `Test/errors/err_unique_brace_init_borrow.cb`, `err_unique_union_field.cb`,
  `err_unique_field_to_field.cb`; positives in `Test/test_move.cb` (`testUniqueField`, 26 -> 42
  after the cleanup pass removed a duplicate; the per-stage counts in this plan drifted and are
  not authoritative - `total += N` in the file is)
  assertions) and `Test/test_collection_leaks.cb` (107 -> 110). Gates: `test.bat` Release all pass,
  `test_lsp.bat` 206/0, `example.bat` 90/0/27 (unchanged from Stage 3).

**Note for whoever picks up the spikes**: ~~`scratch/spike_unique.cb` no longer compiles~~ - STALE
as of Stage 5: the one-line rewrite this note prescribed (`delete p;` -> `p = nullptr;`) has since
been applied to the file, and all three spikes (`spike_unique.cb`, `spike_unique2.cb`,
`spike_doc.cb`) now compile from source and print their PASS line.

### Stage 5 - `unique` joins the interface field contract (finding 1) - DONE 2026-07-16

Implemented Option B exactly as the finding's RESOLVED block settled it. **All three design claims
held, including the one flagged for verification.**

- DONE. **`unique` is spellable on an interface field.** `ParseInterfaceFields` arms
  `StructFieldDeclGuard`, so the field-only position check accepts it. Two additions beyond the
  one-liner the plan budgeted, both load-bearing:
  - It also arms `UnionFieldDeclGuard(inUnionFieldDecl_, false)`. An interface body is never a
    union body, but a generic interface can be instantiated from *inside* one (a union member
    typed `IFoo<int>`), which would otherwise inherit the enclosing body's kind and report the
    union rejection against an interface field. The union rejection itself is untouched and still
    fires (`err_unique_union_field` green).
  - It calls `ValidateUniqueField`, so an interface field gets the same shape rejections a struct
    field does (`unique int x;` is rejected at the interface, not two hops later at the
    implementor). This needed only keeping the `DeclTypeAndValue` that `ParseDeclarationSpecifiers`
    already returns instead of slicing it to `TypeAndValue` on the spot.
  - The ForwardRefScanner copy needed no change: it deliberately does not position-check `unique`
    (the MainListener copy owns the diagnostics `expect_error` observes), so it already set
    `IsUnique` on interface fields.
- DONE. **The contract is enforced, both directions**, via `impl->IsUnique != f.IsUnique` in the
  existing `VerifyInterfaceFields` check. It needed its own message rather than reuse of the type
  one - this is an ownership mismatch, not a type mismatch - so there are two, one per direction,
  each naming which side declared it and both concrete fixes. Ordered after the type check, which
  never returns, so a type mismatch still reports first.
- **CONFIRMED: no access-site plumbing was needed** - the claim the plan asked to verify rather
  than assume. `interfaceFields` is `std::vector<TypeAndValue>`, `TypeAndValue` carries `IsUnique`,
  and `MainListener.h`'s interface-field access site already does `namedVar.TypeAndValue =
  fieldType` + `namedVar.IsInterfaceField = true`. With the flag present, both guards fired with
  zero changes at the store site: Trap A rejects a borrow through the slot, and the reassign-free
  frees the old pointee. `destIsStructField` had already been routed to include `IsInterfaceField`
  for exactly this. Serializer: none needed either - interface fields already round-trip through
  `SerializeTav`/`DeserializeTav`, which carries `IsUnique` as `"uq"`.
- **Beyond the plan, verified**: the flag survives interface INHERITANCE (`CreateInterfaceDefinition`
  flattens a parent's fields into the child's contract by value, so a `unique` field declared on
  the parent still guards through the child's slot - ASAN-clean), and the contract check fires on
  GENERIC interfaces through the monomorphized name (`IBox__Res`). Runtime boxing of a class into a
  *generic*-interface PARAMETER is broken, but it is PRE-EXISTING and unrelated: the identical case
  with no `unique` anywhere fails the same way ("no overload of 'storeGeneric' matches").
- One stale comment corrected: `IsUniqueFieldRead`'s claimed "IsUnique never reaches that path
  today, so it cannot fire there" about `IsInterfaceField`. It does now, by design.
- Consequence, as designed and now enforced: an interface exposing `Res* item` without `unique`
  forbids every implementor from marking it `unique`. Zero fallout - all 69 interface fields in the
  tree are value types (`example.bat` 90/0/27, unchanged).
- Tests: `Test/errors/err_unique_iface_field_impl_only.cb` (the shipped bug's direction) and
  `err_unique_iface_field_iface_only.cb`; positives in `Test/test_move.cb` (`testUniqueField`,
  38 -> 43 assertions) and `Test/test_collection_leaks.cb` (110 -> 112, HeapAudit oracle over 32
  reassignments through the slot). Gates: `test.bat` Release all pass, `test_lsp.bat` 206/0,
  `example.bat` 90/0/27.

## Migration

Opt-in, so nothing breaks. **UNBLOCKED 2026-07-17** - the sole remaining blocker,
[cast-defeats-unique-borrow-tracking](../issue/cast-defeats-unique-borrow-tracking.md), was decided
(Option 1: a cast PRESERVES borrow provenance) and FIXED that day via an out-of-band
`NamedVariable::IsUniqueFieldAlias` flag that survives the cast while `Storage` stays severed (so
`srcIsOwningMove` is untouched). All three rules (`delete`/`move`/store-into-field) now fire on
`(T*)b.p` and on type-changing `(void*)`/`(char*)` casts; four `err_unique_cast_*` tests cover it.
The change is inert until a field is marked `unique`, so `test.bat` 46/0 + `example.bat` 90/0/27 held.
Findings 1, 3, 4, 5 were already closed by Stages 4-5. **Migration can now proceed.**
The candidate set is the **34 verified-owning pointer fields** (out of
254 scalar pointer fields across 144 structs). Each migration adds one keyword and **deletes** a
hand-written destructor.

Do not migrate by field name. The survey found name-based classification misfiles in both
directions: `JsonNode.next` (`json.cb:208`) and `XmlNode._nextSibling` (`xml.cb:211`) look like
classic borrows and are **owning** (`~JsonNode()` at `json.cb:213` deletes the whole sibling chain);
`ArenaAllocator._first_child` owns while its adjacent `._next_sibling` borrows
(`arena_allocator.cb:286-287`, same type, same struct).

## Verification

- `test.bat` (Release) + `test_lsp.bat` + `example.bat` green.
- Error tests in `Test/errors/err_*.cb` via `expect_error` (new files there are sanctioned; the
  no-new-test-files rule does not apply to `Test/errors/`):
  - borrow stored into a `unique` field
  - `unique` on a non-pointer field
  - the D2 recursive-chain rejection
- A positive case in an existing test file: a struct with a `unique` field allocates, destructs, and
  is leak-clean.
- **Leak oracle**: `HeapAudit.enable()` / `reportLeaks()` (needs `-o`) on a migrated type - the whole
  point is that the synthesized dtor frees what the hand-written one used to.
- Run the migration in a second commit, after Stage 1 is green, so a regression is attributable.

## What this does not do

- **Field-only.** `list<unique Node*>` is not a thing. (A `unique Node*` read into a local used to
  be "back to the untracked-borrow rules"; as of Stage 3 the local inherits the borrow, so it
  cannot be `delete`d or `move`d - but it still carries no lifetime, so it can outlive the field.)
- **No lifetimes.** Like C++, a borrow can still outlive its owner. Trap A is caught only in the
  callee, at a field store from a borrowed parameter. Rust's guarantee needs the escape analysis the
  discipline note rules out permanently.
- **Does not touch the value-type path.** `string` keeps its runtime owned bit (`_len` high bit,
  `LLVMBackend.h:2613`, `:2620-2627`). `unique` is a static, compile-time property of a raw pointer
  field. The two mechanisms stay separate and cover different things.

## Appendix: corrections owed to `ownership-move-alias-discipline.md`

Found while verifying this design. That note is labelled "verified, not assumed" but:

1. **Its transfer table is mechanically wrong.** `h->slot = fresh;` does NOT transfer ownership.
   The nulling block (`MainListener.h:8227-8233`) explicitly excludes new-allocated locals escaping
   into a field; what fires instead is a lazy per-variable refcount (`:8190-8215`, init 1, bumped to
   2 on escape) so scope exit decrements to 1 and declines to free
   (`EmitConditionalOwningPtrCleanup`, `LLVMBackend.h:1567-1577`). The end state is right, but
   `fresh` keeps `IsOwning`, is never marked moved, and a later `delete fresh;` is **undiagnosed** -
   a double-free the note does not mention.
2. **Both cited line ranges have drifted.** "cannot delete borrowed parameter" is at
   `MainListener.h:11753-11768`, not 11313 (which is list-element parsing). The call-site `move`
   guard is at `:11985-12002`, not 11553 (which is `new`-ctor argument handling).
3. **`../issue/aligned-new-field-escape.md` is a dangling link** - that issue was fixed by the
   `alignas(slot, alloc)` work and the file deleted. `unified-alignas-slot-alloc.md:41` refers to it
   in the past tense.

Also worth correcting: `doc/LANGUAGE.md:1075` states fields own their pointers. Until Stage 1 lands
that is false; after Stage 1 it is true only for `unique` fields.
