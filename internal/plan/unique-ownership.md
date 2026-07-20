# Unique ownership: fields, locals, params, containers, and interface values

Consolidated 2026-07-19 from two plans, both now deleted:

- `field-ownership-unique.md` (2026-07-16) - Part I below: the `unique` FIELD qualifier.
  Stages 1-5 DONE, including the code-review remediation and the interface field contract.
- `interface-value-ownership.md` (2026-07-18) - Part II below: interface value ownership and
  `unique` in GENERIC-ARGUMENT position (`list<unique X*>`, `list<unique IShape>`), plus
  unique locals and synthesized move params. Stages 1-6 DONE; Stage 7 (D12) was attempted,
  rolled back 2026-07-19, and SUPERSEDED - see "D12: the dead end and why" below.

The corrections and dead-end records in both parts are kept deliberately - they are the
evidence trail for why the design is shaped the way it is. Where Part II superseded a Part I
claim, the Part I text is annotated in place rather than rewritten, so the historical record
stays honest.

## Status and remaining work (the live section)

Last updated 2026-07-20. Everything above the "Completed" ledger is work that is still open;
the ledger is a one-line-per-item record of what landed. The detailed blow-by-blow of each
completed migration was removed on 2026-07-20 to keep this section readable - it is preserved
in git history, and the durable LESSONS from it are hoisted into "Rules that still bind" below.

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

### NEXT - in priority order

1. **Core FIELD migration - unblocked since 2026-07-17, NOT started.** The ~34 verified-owning
   pointer fields (of 254 scalar pointer fields across 144 structs) each gain `unique` and DELETE
   a hand-written destructor. See Part I "Migration" for the cleared blockers and the
   do-not-migrate-by-name warning. The TYPE-ARGUMENT migration (~30 `list<...>` declarations)
   happened in Part II Stage 5; the FIELD migration did not.

2. **Remove `alias` as a generic type ARGUMENT.** Ruled 2026-07-19: once bare means borrow,
   `list<alias T*>` and bare `list<T*>` mean the same thing, so the type-argument form is
   redundant surface area. **`alias` on PARAMETERS and RETURNS must STAY** - it is load-bearing
   there (`get()`, `peek()`, and the `dequeue` design above all depend on it). Do this only
   after every container has landed, so nothing is mid-migration when the spelling disappears.

3. **`unique` field-shape rule is skipped under generic substitution - STILL OPEN, and the
   originally filed fix direction is WRONG.** See
   `internal/issue/unique-field-check-skipped-on-substituted-generic-type.md`. Re-validating at
   monomorphization would reject `btree<K, unique C*>` and `btree<K, unique IFace>`, both of
   which hand-write teardown and are correct today. What is missing is the DISTINCTION ("is this
   field's release synthesized?"), not the check. The live hole is a generic struct with a
   `unique V values[N]` field and NO hand-written teardown: nothing diagnoses it, nothing frees
   it. Two fix directions are recorded in the issue file; both are breaking for `btree` and
   neither has been attempted.

4. **Optimisation, not a leak:** `list<string>` named-lvalue sites still deep-copy where the
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
   - *field-shape rule skipped under substitution* - STILL OPEN, and the filed fix direction is
     WRONG. See `internal/issue/unique-field-check-skipped-on-substituted-generic-type.md`:
     re-validating at monomorphization would reject `btree<K, unique C*>` and
     `btree<K, unique IFace>`, both of which hand-write teardown and are correct today.

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

- **`&&` and `||` do NOT const-fold** (`internal/issue/if-const-no-constant-folding-path.md`).
  Every compile-time condition MUST be a chained `else if const`. This is why `is_interface(T)`
  had to exist at all - "pointer AND NOT interface" cannot otherwise be spelled.
- **Declare the PLAIN overload BEFORE the `move` one.** Interface conformance matches the FIRST
  declared overload (`internal/issue/interface-conformance-matches-first-overload.md`), and the
  diagnostic on failure is confident and points at the wrong fix.
- **The `if const (!is_pointer(T))` guard on a transfer overload is DESIGN, not a workaround.**
  A borrowing container must not accept ownership of a pointer element - nothing would ever free
  it. For `unique T*` the plain parameter is already a synthesized move sink (D4).
- **Never add an `is_alias` arm.** `is_pointer(T)` is true for bare, `unique`, `alias` AND
  interface types, so it subsumes the alias case; an `is_alias` arm is unreachable dead code.
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
- **`alias` is pointer/interface only.** `list<alias string>` is rejected. The `unique`/`alias`
  symmetry is deliberately incomplete for value types - a borrowed string has no representation.
  Document it; do not "fix" it.

### Deferred by maintainer ruling - do not action as part of the above

- **Lifetime / dangling for borrowed containers: ignored, none planned.** A `list<T*>` outliving
  its owner is undetectable. The hazard pre-existed unnamed; borrow-by-default makes it the
  blessed pattern, which raises the stakes but does not change the risk.
- **Blocker 1 remains unresolved, but is DORMANT.** A plain by-value param still BORROWS an
  owning value while local init MOVES it. Nothing currently exercises the gap, but the asymmetry
  is still in the language and will resurface for the next generic that stores a value param.
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

- `delete` on a unique LOCAL reports a message worded "unique field" (Part II Stage 3).
- Part I "Non-blocking cleanups": dead `TypeOwnsUniquePointer` bail in
  `GetOrCreateMemberwiseCopy`; Trap A prints `unique field ''` for bare self-field access;
  `unique int x[4]` reports a pointer-flavored message; one duplicate test in `test_move.cb`; a
  stale "frees and nulls" comment; `std::set` vs `unordered_set` inconsistency. Not re-verified
  since 2026-07-16 - some may have been cleaned up in passing.

### Open issues owned by this workstream

Filed and NOT fixed. Container work is done; these are the compiler-side remainder.

| File | What |
|---|---|
| `unique-field-check-skipped-on-substituted-generic-type.md` | field rule skipped under substitution |
| `duplicate-add-leaks-unique-value.md` | `dictionary<K, unique V*>.add()` leaks on a duplicate key |
| `move-return-named-struct-local-leaks-fields.md` | `return move r;` leaks a struct local's owning FIELDS |
| `user-copy-method-result-leaks-in-container.md` | a hand-written `copy()` returning `move v` leaks once stored |
| `interface-conformance-matches-first-overload.md` | conformance matches first declared overload |
| `compile-error-non-literal-emits-garbage.md` | `compile_error(CONST)` emits a stray character |
| `if-const-no-constant-folding-path.md` | `&&` / `||` do not const-fold |
| `if-const-global-condition-crash.md` | `if const` on a global condition crashes |
| `delete-borrow-via-named-local.md` | `delete` on a borrow via a named local is not rejected |
| `generic-interface-explicit-type-arg-base-clause.md` | explicit base-clause form drops `*` / `unique` |

### Completed ledger

| Done | What | Where |
|---|---|---|
| 2026-07-16 | Part I stages 1-5: `unique` FIELD qualifier, assignment discipline, Trap B, code-review remediation, interface field contract | Part I below |
| 2026-07-18/19 | Part II stages 1-6: interface value ownership, `unique` in generic-argument position, unique locals, synthesized move params (D4), `dictionary`/`hashset` unique support | Part II below |
| 2026-07-19 | D12 ATTEMPTED AND ABANDONED - superseded by the `alias` type argument | "D12: the dead end and why" |
| 2026-07-19 | `alias` as a generic type argument LANDED (compiler only). `IsAliasTypeArg` deliberately separate from `IsAlias`; serialized as `o["alt"]` | - |
| 2026-07-19 | Prerequisites: both move-overload defects FIXED, `is_copyable(T)` landed, `is_pointer(IShape)` flipped TRUE | - |
| 2026-07-19 | Bare `list<T*>` flipped to OWNING - then REVERSED by the borrow-by-default ruling | superseded |
| 2026-07-19/20 | **`list` migrated to borrow-by-default**, `IList<T>` reshaped with no `move`, `take()` removed, six code-review findings fixed | commit `6528268` |
| 2026-07-20 | **`hashset` + `dictionary` migrated.** Fixed a compiler bug: the move/borrow tie-break ran only on the perfect-match tier, so a LITERAL key made `d.add(1, x)` silently consume `x` | commit `7f5d433` |
| 2026-07-20 | **`hpc/btree` migrated** - the last container on its own convention. Fixed a real duplicate-key bug that freed the caller's value. `_freeValue`/`_freeKey`'s move-param workaround SURVIVED and had to (its body is now EMPTY - the param's scope-exit destructor IS the release) | uncommitted |
| 2026-07-20 | **`array` migrated** | uncommitted |
| 2026-07-20 | **`queue` + `stack` migrated** - `_placeAt`/`_releaseAt`, plain + guarded-`move` insert, `dequeue()`/`pop()` return kind selected by member-scope `if const` (`alias` for a borrowed element), release-walk destructor. Fixed a pre-existing `queue._grow()` capacity bug: `if (_size >= _capacity)` ignored `_front`, so enqueueing after a partial drain wrote PAST the buffer | uncommitted |
| 2026-07-20 | **`unique IFace` cluster: 2 of 3 FIXED** (+ a third, unrelated bug found with them). Owning fixed-array locals were never walked at scope exit (`unique C*[4]` leaked identically - never interface-specific); substitution set `IsUniqueTypeArg` on a `V* out` param so a POINTER TO the owning location read as a sink and dangled; `IsOwningInterfaceValue` tested only `IsUnique`, which substitution never sets, so `btree<K, unique IFace>` freed nothing. Unblocked `btree<K, unique IFace>` | uncommitted |
| 2026-07-20 | **Interface array FIELD sizing bug FIXED** - `IFace v[N]` allocated ONE element with a correct 16-byte stride, silently clobbering everything after it. Plus a second bug found while verifying: the subscript handler never refreshed `interfaceVar`, so `a[i].method()` dispatched off the stale base address (affects LOCALS too) | uncommitted |

---

## D12: the dead end and why

Preserved verbatim in substance from the pre-2026-07-19 live section. D12 is SUPERSEDED
(see live item 2) and must NOT be implemented, but the evidence trail explains why `alias`
is shaped the way it is, and it records three claims that were WRONG and cost real time.
Do not re-derive any of them.

D12's ruling was: per element kind, unique T => synthesized move (D4); owning value struct
=> auto-move; primitive => copy; `string` => COPY; bare pointer/interface => BORROW.
Implementation found it rested on premises that do not hold.

**PREMISE ERROR - `string b = a` MOVES, it does not copy.** D12 justified "string => COPY"
as "consistent with `string b = a`". Verified: `string b = a;` consumes `a`
(use-after-move on the next read), and `core/string.cb`'s own header documents "`string` is
move-by-default". The semantics CONSISTENT with the language is TRANSFER, not copy. Under
`alias` this leg disappears entirely: `l.add(s)` moves, `l.add(s.copy())` copies, no
call-site `move`, no copy-by-default.

**BLOCKER 1 - a plain (non-`move`) value param BORROWS an owning value.** A plain `string`
/ owning-struct / closure param does not take ownership, is not destructed at scope exit,
and shallow-shares the caller's buffer. Local INIT auto-moves owning values; param PASSING
does not. So an insert body doing `_data[slot] = move value` hands one buffer to two owners
for a named lvalue argument (verified double-free/abort), and an owned RVALUE temp is
aliased into the slot while the caller's end-of-expression flush still frees it (verified
dangling/garbage reads). STILL UNRESOLVED but DORMANT - see live item 5.

**BLOCKER 2 - move-overload discrimination.** The original claim ("never resolves") was
wrong. The 2026-07-19 correction ("works for pointers, not value types") was ALSO WRONG and
is retracted - see live item 5 and
`internal/issue/move-ptr-arg-does-not-select-move-overload.md`. Actual behavior: the
pointer pair is fully INVERTED (a plain call selects the MOVE overload and frees; a `move`
call selects the BORROW overload and leaks). The "borrow=1 move=1" evidence that looked
like success was a COUNTING oracle, which cannot distinguish "both correct" from "both
swapped". **Any future verification here must assert WHICH overload ran, never call
counts.**

**UPDATE 2026-07-19 (later the same day) - the VALUE-type half is now FIXED.** This entry
previously said the value pair "silently selects the plain overload both ways". Re-measured
after the silent-consume fix landed, with a canonical pair `f(string)` / `f(move string)`:
`f(a)` selects BORROW and `f(move b)` selects MOVE - correct in both directions. The
POINTER pair is still fully inverted, unchanged. So the current state is split:

| element kind | `f(x)` | `f(move x)` | verdict |
|---|---|---|---|
| value (`string`) | BORROW | MOVE | correct |
| pointer (`Res*`) | MOVE | BORROW | still INVERTED |

This has a direct library consequence, hit while building `cflat/core/vector.cb`: a
container CANNOT offer a `push_back(T)` / `push_back(move T)` pair for pointer elements,
because the plain call silently consumes the caller's pointer. `vector.cb` guards its move
overload with `if const (!is_pointer(T))` and documents the guard as a workaround for the
bug, to be removed when the pointer path is fixed.

**DISPROVED - the two-pass agreement blocker never existed.** An earlier Stage 7 attempt
withheld its D4-synthesis work claiming `ForwardRefScanner` "doesn't apply
`activeTypeSubstitutions`, so the two passes disagree on `IsMove`". Structurally
impossible: `activeTypeSubstitutions` is declared INSIDE `MainListener`; the scanner has no
such member and bails on generics outright (generic templates get LSP symbol registration
only - no `ParseParameterTypeList`, no `CreateFunctionDeclaration`). For a generic
container method the pre-pass registers NO signature, so there is nothing to disagree with.
Struck; do not re-inherit it.

**VALIDATED by spike** (patch preserved at `scratch/d12-pointer-leg-spike.patch`, 3 files /
~19 lines). Post-substitution D4 synthesis worked and BOTH pointer legs were correct from
ONE plain-`T` declaration: `list<unique Res*>.set` transferred (dtor=1 after set, dtor=2 at
scope exit, leaks=0); bare `list<Res*>` borrowed (dtor=0, caller deletes once, leaks=0);
monomorphization gave distinct symbols from the same source. ForwardRefScanner needed
NOTHING. The mechanism: an `inParamDecl_` flag plus, in the codegen
`ParseDeclarationSpecifiers` substitution branch,
`if (substUnique && inParamDecl_ && !declType.IsAlias) declType.IsUnique = true;`.

**What actually blocked it: the owning-VALUE leg only.** With insertion made plain `T` the
suite went 408/11/8. Nine failures were one fault - Blocker 1 - with a uniform backtrace
(`operator delete` <- `string.dtor` <- `~list__string`). The other 2 were
`err_aligned_new_escape` case 3, whose premise D12 superseded.

**A second spike** (`scratch/d12-set-three-leg-spike.patch`) used member-scope `if const` to
write three legs directly in `list.cb`. It WORKED end to end, including differing move-ness
in the `IList<T>` contract with virtual dispatch on a linked binary. **Rejected on
language-health grounds by the maintainer:** `if const` in a BODY is a fine implementation
detail, but `if const` in a SIGNATURE or interface CONTRACT makes a type's public shape
conditional - the contract becomes a contract generator. It also pushes a correctness
burden onto every container author (no exhaustiveness check; a future `shared T` would
silently fall into the `else` leg of four containers independently) and degrades
diagnostics. C++ deliberately cannot do this: `if constexpr` cannot change a signature.
**Ruling: `if const` should be limited to platform or external concerns.**

**Options considered and rejected** (recorded so they are not re-proposed):
- **(a) move-ness as a value-type overload discriminator** - rejected as the container
  mechanism (C++'s `const T&`/`T&&` boilerplate reborn, doubled per method per container).
  The capability itself IS wanted - see live item 5.
- **(b) own-by-value params** - a plain value param takes ownership and destructs at scope
  exit, matching local init. This is the C++ model and removes Blocker 1 at the root. Costs
  a choice between smuggling implicit deep-copy into a language whose identity is no
  implicit copy, or consuming the argument of every existing by-value `string` function in
  the tree. Blast radius never measured.
- **(c) extend the promotion synthesis to owning value types** - never spiked. Carries a
  provenance smell (a substituted `T` sinks while a concrete `string` param borrows) plus
  an unresolved risk that `GetOrCreateFullDestructor(T) != nullptr` may not resolve at
  parameter-parse time.
- **(d) "demotion"** - keep `move T` and demote it to a borrow when T substitutes to a
  non-owning type. **Rejected on the provenance test:** two monomorphized functions with
  byte-identical signatures would behave differently depending on whether the author wrote
  `move void*` or `move T` with T=`void*`, breaking the rule that instantiating a template
  at X behaves like hand-writing it at X. It also made a legitimate design (a generic
  container that DOES own bare pointers) unwritable.
- **An advisor's "finish move's definition"** - make move-binding of any non-owning type
  (primitive, bare pointer, bare interface) a copy, uniformly. Elegant and provenance-free,
  but **its premise is false in cflat**: bare pointer does NOT imply non-owning. D9 permits
  hand-managed raw-pointer ownership and core relies on it - `core/json.cb:225`
  `_appendChild(move JsonNode* child)` is a bare pointer declared as a sink whose tree
  `delete`s children at `:213`, and the same shape appears in `numa.cb`, `string.cb`,
  `process.cb` and throughout `threadpool.cb`. The rule would have made all of them
  double-free or leak. `alias` routes around this because the weakening is OPT-IN rather
  than inferred from pointer-ness.

# Part I - the `unique` field qualifier

Created 2026-07-16. **Stages 1-5 DONE (2026-07-16).** Both open decisions settled
(2026-07-16): the keyword is `unique`, and recursive chains are rejected. See Decisions.

Complements [ownership-move-alias-discipline](ownership-move-alias-discipline.md), which
stays correct for *parameters*; this part addresses *fields*, which that note does not cover
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

That loop is otherwise exactly C++ memberwise destruction: it asks "does this field's TYPE
have a destructor?" and nothing else (`IsOwningValueType`, `LLVMBackend.h:2846-2853`, is
literally `GetOrCreateFullDestructor(typeName) != nullptr`). It is why `string` fields
already auto-destruct.

So before this work, ownership of a pointer field existed **only** as a hand-written
destructor plus a comment. Consequences:

- A pointer field with no hand-written dtor leaks, silently, forever.
- `doc/LANGUAGE.md:1075` claimed "struct fields own their pointers". The compiler did not
  implement that - the doc described a convention, not a behavior.
- `ClosureCaptureDeepCopyable` (`LLVMBackend.h:2887-2897`) bails on **any** pointer field,
  because own-vs-borrow is unknowable.

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

**The keyword is opt-in and additive.** An unmarked raw `T*` field behaves exactly as before
(never auto-freed). This is the property that makes the change affordable - every hard case
opts out by doing nothing:

| Hard case | Resolution |
|---|---|
| `json.cb` / `xml.cb` arena-vs-heap duality (`firstChild` owns on the heap path, borrows on the arena path - `json.cb:390-394`, `:405`) | does not use `unique`; keeps its hand-written dtor. Unchanged. |
| `ComPtr<T>` (`com.cb:129`) - owns, but releases via `lpVtbl->Release`, not `delete` | does not use `unique`. Unchanged. |
| ~76 OS-handle fields (`File._handle`, `Socket._handle`, ...) - freed by `CloseHandle`/`munmap` | does not use `unique`. Unchanged. |
| ~46 C-ABI layout mirrors (`ui_native/win32.cb`, `process.cb:60-96`) | must stay raw for layout. Unchanged. |
| Container element ownership | at the time of this design, "untouched - `unique` is field-only and does not redefine raw `T*`". SUPERSEDED by Part II: element ownership is now explicit via `unique` type arguments, and bare `list<T*>` became borrowed. The affordability argument here predates that breaking change. |

Alternatives rejected at the time:

- **Rejected: `alias` field qualifier with owning-by-default.** Taxes the common case (~84
  borrow fields would need annotating), and "owning by default" would be a claim the compiler
  does not back with a free. Also cannot express the arena duality.
- **Rejected: `box<T>` owning pointer type (the C++/Rust shape).** Cleaner in principle -
  ownership in the type, composes into generics - but at the time it redefined raw `T*` as
  "borrow" globally, contradicting the then-shipped container owned-model, and drags in a
  `new`-produces-a-box story plus `operator->` ergonomics. (Part II later adopted the
  borrowed-by-default model for CONTAINERS deliberately, with a migration; the box-wrapper
  alternative was re-examined and re-rejected there on different grounds - see Part II
  "Prior art".)

### Precedent

`gsl::owner<T*>` (C++ Core Guidelines) is exactly this marker on a raw pointer, invented
because `unique_ptr` cannot be retrofitted into C-ABI and legacy layouts. Objective-C ARC's
`__strong`/`__weak` are the same idea as declaration qualifiers. Both are *inert* - they only
feed static analyzers. `unique` goes further by synthesizing the destruction.

### Why the plumbing is cheap

1. **No grammar change.** `move` / `alias` / `bond` already parse on fields as soft keywords
   and land in `StructFields[i].IsMove` / `.IsAlias` / `.IsBond`. Text-match in both
   `ParseDeclarationSpecifiers` copies (`MainListener.h:739` ForwardRefScanner, `:2347`
   codegen; `alias` at `:744`/`:2352`), per the CLAUDE.md soft-keyword rule. Do NOT add to
   the ANTLR lexer.
2. ~~**Per-field properties already flow to the access site** via the `AllocAlignValue`
   route.~~ **WRONG - no route needed (verified 2026-07-16).** `IsUnique` lives on
   `TypeAndValue`, and `MainListener.h:13290`/`:13434` bind
   `fieldType = StructFields[fieldIndex]` then `namedVar.TypeAndValue = fieldType`, so the
   flag is already present at the assignment site. The `AllocAlignValue` route exists because
   that value is copied onto a *separate* `NamedVariable::AllocAlignment` member. A budgeted
   step that turned out to be a no-op.
3. **Composes with `alignas(slot, alloc)`.** Both are field qualifiers; the synthesized dtor
   needs `AllocAlignValue` to free an over-aligned allocation correctly.

### LOAD-BEARING: the `--init` serializer

`unique` adds a field an analysis reads. It **MUST** be in the cache round-trip in the same
change, or it is silently dropped on a warm cache and the `expect_error` tests stop firing.
See `internal/testing-notes.md`.

DONE (Stage 1): `IsUnique` sits on `TypeAndValue` next to `IsMove`/`IsBond` (NOT on
`DeclTypeAndValue` as first written - `DeclTypeAndValue` derives from it, and `SerializeDtav`
delegates to `SerializeTav`, so one line each way covers struct fields too). Key `"uq"`,
following `IsMove`'s `"mv"`, in `SerializeTav`/`DeserializeTav`
(`LLVMBackend.cpp:3773`/`:3826`).

Corrections to this section as originally written: **there is no serializer in
`LLVMBackend.h`** - the cited `:15577`/`:15607` never existed (unverified line refs carried
in from an earlier session). The round-trip lives only in `LLVMBackend.cpp`.

## Decisions (settled 2026-07-16)

### Part I D1 - keyword name. SETTLED: `unique`.

| Candidate | For | Against |
|---|---|---|
| **`unique`** | pairs with `alias` (already means "you do not own this, do not free it") on the same axis, opposite pole; short, like `move`/`bond` | cflat uses alias/noalias vocabulary for the OPTIMIZER (`span<T>` noalias vs `view<T>` may-alias, `NoaliasScopeId`). Could be misread as a `restrict`-style promise. Judged mild: `alias` is already an ownership word in cflat, not an optimizer word |
| `owner` | `gsl::owner` pedigree | **collides in-tree**: `_BA_PageHdr._owner` / `_BA_LargeHdr._owner` (`bucket_allocator.cb:48`, `:76`) are back-pointers meaning "the allocator that owns ME" - the opposite direction. `owner BucketAllocator* _owner;` reads as a contradiction |
| `owned` | dodges both collisions | adjective attaches to the wrong noun (the field is not owned; the pointee is) |

### Part I D2 - recursion. SETTLED: reject. A recursive structure owns its own teardown.

**Rule: a recursive data structure cannot use `unique`. Its author designs the teardown.**
Confirmed 2026-07-16 after a full re-examination; the Stage 1 implementation already does
this.

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

## Part I stages (all DONE)

### Stage 1 - parse and destruct - DONE 2026-07-16

- Text-match in both `ParseDeclarationSpecifiers` copies; `TypeAndValue::IsUnique` (which
  `DeclTypeAndValue` inherits, so it reaches `StructFields` with no extra plumbing).
- Serializer round-trip (`"uq"`) in `SerializeTav` / `DeserializeTav`.
- `GetOrCreateFullDestructor` collects `unique` pointer fields ahead of the pointer skip and
  emits a null-checked delete via `EmitUniqueFieldDelete`, honoring `AllocAlignValue`
  (`__delete_aligned` when over-aligned). The pointee dtor binds through
  `GetFullDestructorForDelete`, so an incomplete pointee is not baked out as null. A `unique`
  field is pushed even when the pointee is trivially destructible - the block still needs
  freeing.
- Position was enforced with a scoped flag: `ParseDeclarationList` arms `inStructFieldDecl_`
  around its specifier parse, so `unique` on a local / parameter / return type was rejected
  by fall-through. **SUPERSEDED by Part II D3/D4:** locals and params are now legal (a unique
  param IS a synthesized move param); return position stays rejected. The two err_ tests
  asserting the field-only restriction were deleted in Part II Stage 3.
- Rejections: non-pointer, `T**` (later relaxed by Part II D2's whole-type ruling for
  type-argument position), fixed array, array-view, simd, bitfield, non-field position (see
  above), and the D2 cycle. All `LogErrorContext` (or `LogError` for the cycle walk).
- D2 uses a registration-time transitive walk (`RejectUniqueDestructionCycles` at
  `CreateStructType`), not the `fullDestructorInProgress_` fallback: it fires regardless of
  whether the type is ever destructed. Transitive (self, mutual, N-hop); reports from
  whichever member is registered last - the one that closes the chain.
- Tests: `Test/errors/err_unique_*.cb` (8 at the time), positives in `Test/test_move.cb`
  (`testUniqueField`, dtor-count oracle) and `Test/test_collection_leaks.cb` (HeapAudit).

### Stage 2 - assignment discipline - DONE 2026-07-16

- **Reassignment destructs first.** The assignment site calls the same
  `EmitUniqueFieldDelete` the synthesized destructor uses, so a field is freed identically at
  teardown and at reassignment. `nullptr` frees the old pointee (the sanctioned
  release-early idiom); `new T()` frees and re-roots. Honors `AllocAlignValue`. Proven by the
  HeapAudit oracle: 100 reassignments leak 0, while the same loop on an unmarked pointer
  field leaks 10/10.
- **Self-assign is guarded at RUNTIME**, not by name. `EmitUniqueFieldDelete` gained an
  optional `replacement` argument (the pointer about to be stored) and skips the free when
  the field already holds it. Name matching is not sufficient: a bare `item = item` inside a
  method resolves through `GetMemberVariable`, which deliberately leaves
  `FieldName`/`OwningStructName` empty (`MainListener.h:17538`) so the delete-encapsulation
  rule does not fire on self-access. SSA comparison fails too (each access re-loads `this`).
  The runtime compare also covers a source that merely aliases the field.
- **Storing a borrow into a `unique` field is an error** (Trap A), via `rightNV.IsBorrowed`
  next to the sibling rejections. Two messages: the borrowed-parameter case, and the case
  where the source is a field reached through a borrowed parameter.
- **A struct with a `unique` field is not memberwise-copyable.** The real gap was NOT
  `Holder b = a;` (that already auto-MOVES, with use-after-move enforced) nor a
  field-to-field copy (already rejected via `IsOwningValueType`). It was the synthesized
  `.copy()`, whose skip-list shallow-copies pointer fields - ASAN-confirmed double-free,
  transitively through a by-value member too. Closed with `TypeOwnsUniquePointer`
  (transitive), which bails `GetOrCreateMemberwiseCopy` and reports at the copy-synthesis
  choke point.
- **`delete` on a `unique` field is rejected** - a double-free Stage 1 opened, since the
  synthesized dtor runs the user dtor first and then deletes the field. Keyed on `IsUnique`
  rather than `FieldName` so the bare self-field form is caught.
- Tests: `err_unique_borrow_into_field.cb`, `err_unique_delete_field.cb`,
  `err_unique_memberwise_copy.cb`; positives in `test_move.cb` and
  `test_collection_leaks.cb`.

**Uninitialized-slot hazard: investigated, slot is trusted.** Every scalar construction path
zero-initializes: `new T()`, `new T[n]`, stack locals, and user-ctor entry all run a
synthesized default ctor that returns `zeroinitializer` (verified in IR and by poisoning +
forcing heap reuse). The only hole is `malloc` + cast, which bypasses construction entirely -
and that ALREADY crashes the synthesized destructor on the same slot, and breaks a `string`
field identically. Out of contract, not a `unique`-specific hazard. Requiring an initializer
(`unique Node* p = nullptr;`) was considered and REJECTED: `malloc` bypasses field
initializers too, so it would not close the hole - pure ceremony.

**CORRECTION 2026-07-16: "every construction path" was overstated - a FIXED ARRAY OF STRUCTS
was not reliably zero-initialized.** FIXED 2026-07-18 (`EmitFixedArrayDefaultInit` in
`MainListener.h` default-inits fixed-array locals, mirroring the scalar path; the issue file
was deleted). Historical note kept because the false lead is instructive: an earlier version
claimed the behaviour varied by element type (hand-written dtor zero-inits, synthesized does
so for `unique` but not `string`). **That was noise** - it was reading whatever stack garbage
was present, and the results inverted between runs. The rule was simply "arrays are never
initialized, scalars always are".

### Stage 3 as designed (arrays and precision) - CANCELLED 2026-07-16: all three items dead

Every item was verified before building. All three failed. **The provenance gradient is the
lesson, and it held for later stages too:** Stage 1 (written from exercised code) matched its
design almost exactly; Stage 2 (written from read code) had three of four claims fail on
contact; Stage 3 was 0 for 3.

- ~~`unique Node* children[17]` fixed-array support~~ **DEAD: no motivating case.**
  `btree_node.children[17]` is the ONLY fixed array of pointer fields in the tree (verified
  by sweep), and it is recursive, so under D2 it can never use `unique` regardless.
- ~~Sharpen `ClosureCaptureDeepCopyable` to bail only on `unique` fields~~ **DEAD: would
  introduce a double-free.** The bullet silently assumed an unmarked `T*` field is a borrow.
  Not true: the ~34 verified-owning unmarked pointer fields own via hand-written dtors.
  Sharpening would flip their closure capture to by-VALUE, shallow-copy the owned pointer,
  and double-free. The bail is load-bearing.
  **CORRECTION - the spike evidence originally cited here was FALSE.** The spike's destructor
  nulled the pointer after deleting, masking the second free. Isolated A/B: `{ delete p;
  p = nullptr; }` freed once; `{ delete p; }` (the `json.cb` / `ComPtr` shape) was an ASAN
  double-free. By-reference capture does NOT prevent double destruction - the baseline was
  NOT safe. (That baseline bug, closure-capture-owning-struct-double-free, was then FIXED
  2026-07-16: the lambda invoker body registered the capture as an ordinary named local
  without `IsAliasBorrow`, so `EmitDestructorsForScope` destructed the caller's struct on
  every call. One line: `captureNV.IsAliasBorrow = true`, `MainListener.h:17033`.)
- ~~Sharpen the memberwise-copy skip-list to deep-clone unique pointees~~ **DEAD: the blunt
  behavior IS the correct semantic.** `std::unique_ptr` is deliberately non-copyable and
  move-only; a struct holding one is too. Auto-deep-cloning would silently turn an intended
  move into a deep copy.

### Stage 3 as shipped - Trap B through a `unique` field - DONE 2026-07-16

Laundering a `unique` field through a plain local defeated every Stage 2 check and
double-freed (ASAN-confirmed). NOT a regression - the hand-written `~Box() { delete p; }`
equivalent always had it - but `unique` made it reachable with less code.

- **A local bound from a `unique` field inherits the borrow.** One new source-classification
  clause at the `ParseDeclaration` init site, plus `NamedVariable::BorrowedUniqueField`
  ("Struct.field") so diagnostics name the real owner. Reads/writes through the alias stay
  legal; only the free is rejected. The flag rides `NamedVariable`, which is **not**
  serialized, so no `--init` change.
- **Both spellings are closed.** The canonical Trap B in
  [ownership-move-alias-discipline](ownership-move-alias-discipline.md) is
  `consume(move alias)` - fixing only `delete` left it open (ASAN-confirmed) while the new
  delete diagnostic actively steered users toward `move`. `move` on a unique-field-derived
  local is rejected in `ParseMoveExpression`. Forwarding an ORDINARY borrow as `move` stays
  legal: that rule rests on the programmer asserting the borrow is dead, which for a `unique`
  field can never hold - the field's destructor is synthesized and *will* run. Narrowly keyed
  on `BorrowedUniqueField`.
- `move b.p` remains the sanctioned extraction: it returns a fresh NamedVariable with no
  `Storage` and sets `lastOwningResult`, so it never reaches the new clause.
- **No regression on the ~254 ordinary pointer fields** (keyed on `TypeAndValue.IsUnique`;
  pinned by a `plain_field_alias_delete_still_legal` case; `example.bat` 90/0/27).
- Tests: `err_unique_delete_alias.cb`, `err_unique_move_alias.cb`,
  `err_unique_alias_into_field.cb`; positives in `test_move.cb` and the HeapAudit oracle.

The direct field-to-field copy (`c.p = a.p;`) found here was assessed a MIGRATION BLOCKER
(the longer suspicious spelling errored while the shorter natural one double-freed) and was
closed in Stage 4 remediation, finding 4 below.

### Code review 2026-07-16 - findings and remediation

Two adversarial reviews (memory-safety, integration) plus a maintainer pass over the full
388-line diff. **No regression to ordinary code** - zero `uq.*` markers in non-`unique` IR,
byte-identical dtor collect loop for ordinary pointer fields, serializer round-trip correct.
Every finding was a hole in the feature's OWN rules. All five are now CLOSED:

1. **Interface field slot: both guards absent, no correct spelling existed.** A `unique`
   field reached through an interface's byte-offset slot got neither the reassign-free
   (leak) nor Trap A (double-free), and `unique` was not spellable on an interface field.
   **RESOLVED: `unique` joins the interface field contract** (Stage 5 below). Not fixable by
   propagating the implementor's flag - through dynamic dispatch the concrete type is
   unknown, and two implementors could disagree; ownership either lives in the CONTRACT or is
   not expressible across an interface. Deciding evidence: the interface contract already
   carries ownership and it works (`IChannel.push(move T item)`, `ICopyable.copy()` returning
   `move TSelf`; `move` verified to transfer correctly through dynamic dispatch). Rejected
   alternative - forbidding `unique` through interface slots - was dropped because
   ownership-in-the-contract is shipped design, not speculation. Accepted consequence: an
   interface exposing `Res* item` without `unique` forbids every implementor from marking it
   `unique`; implementor-owns + interface-silent IS this bug.
2. **A `(T*)` cast defeated all three Stage 3 rules.** FIXED 2026-07-17 after the maintainer
   ruling (Option 1: a cast PRESERVES borrow provenance), via an out-of-band
   `NamedVariable::IsUniqueFieldAlias` flag that survives the cast while `Storage` stays
   severed (so `srcIsOwningMove` is untouched). All three rules fire on `(T*)b.p` and on
   type-changing `(void*)`/`(char*)` casts; four `err_unique_cast_*` tests.
3. **Brace-init / element sugar bypassed Trap A.** `EmitOneFieldInit` is a SECOND independent
   field-store path (brace-init + `<Tag attr=...>` sugar). FIXED in Stage 4 remediation: both
   source rejections extracted as shared helpers `RejectBorrowIntoUniqueField` and
   `RejectUniqueFieldToUniqueField`, called from both paths so they cannot drift. Note the
   plan said "Trap A only" - not enough: brace-init reaches the same path with a
   field-to-field source, so finding 4's rule was needed there too. (This was instance #N of
   the store-path-parity class - see `internal/issue/brace-init-field-store-not-at-parity.md`
   for the durable rule: new field-store rules MUST be shared helpers from the start.)
4. **Direct field-to-field copy `c.p = a.p`.** FIXED in Stage 4 remediation: new store-site
   rule keyed on `namedVar.TypeAndValue.IsUnique && IsUniqueFieldRead(rightNV)`.
   `IsUniqueFieldRead` requires the source's `Storage` to be a 2-index struct GEP - which
   keeps `move a.p` legal (fresh NamedVariable, no `Storage`) and keeps plain locals out.
   Ordered AFTER Trap A so a borrowed-parameter source reports the more precise reason.
   `selfUniqueFieldAssign` also compares declared field names when both sides are bare
   (`GetMemberVariable` leaves `FieldName` empty by design); the runtime `replacement` guard
   still does the real work.
5. **`union` bypassed D2, and unique-in-union was an arbitrary free** (freed regardless of
   the active member). FIXED in Stage 4 remediation: `unique` on a union member is rejected
   in `ValidateUniqueField`, ahead of the shape checks, armed by `UnionFieldDeclGuard` (which
   carries the body's own kind and restores on exit - a struct nested in a union keeps
   `unique`; a union nested in a struct still rejects). **This subsumes the `CreateUnionType`
   D2 gap, verified rather than assumed**: `UniqueChainReaches` traverses only `IsUnique`
   pointer fields, and no union member can carry `IsUnique` (the C-interop `CreateUnionType`
   caller builds fields from clang's AST and never sets it). Unions destructing inactive
   members remains a PRE-EXISTING separate hazard.

Stage 4 (remediation, findings 3-5) DONE 2026-07-16. Tests: `err_unique_brace_init_borrow.cb`,
`err_unique_union_field.cb`, `err_unique_field_to_field.cb`; `testUniqueField` grew to 42
assertions (per-stage counts in the old plan drifted; `total += N` in the file is
authoritative). Gates: `test.bat` Release all pass, `test_lsp.bat` 206/0, `example.bat`
90/0/27.

### Stage 5 - `unique` joins the interface field contract (finding 1) - DONE 2026-07-16

All three design claims held, including the one flagged for verification.

- **`unique` is spellable on an interface field.** `ParseInterfaceFields` arms
  `StructFieldDeclGuard`. Two additions beyond the budgeted one-liner, both load-bearing:
  it also arms `UnionFieldDeclGuard(inUnionFieldDecl_, false)` (a generic interface can be
  instantiated from *inside* a union member and would otherwise inherit the enclosing body's
  kind), and it calls `ValidateUniqueField` so an interface field gets the same shape
  rejections at the interface, not two hops later at the implementor. The ForwardRefScanner
  copy needed no change.
- **The contract is enforced, both directions**, via `impl->IsUnique != f.IsUnique` in the
  existing `VerifyInterfaceFields` check - with its own two messages (ownership mismatch, not
  type mismatch), ordered after the type check.
- **CONFIRMED: no access-site plumbing needed.** `interfaceFields` is
  `std::vector<TypeAndValue>`, which carries `IsUnique`; the access site already does
  `namedVar.TypeAndValue = fieldType` + `IsInterfaceField = true`, and `destIsStructField`
  had already been routed to include `IsInterfaceField`. Both guards fired with zero store-
  site changes. Serializer: interface fields already round-trip through `SerializeTav`.
- **Beyond the plan, verified:** the flag survives interface INHERITANCE
  (`CreateInterfaceDefinition` flattens parent fields by value - ASAN-clean) and the contract
  check fires on GENERIC interfaces through the monomorphized name. (Runtime boxing of a
  class into a *generic*-interface PARAMETER is broken but PRE-EXISTING and unrelated.)
- Tests: `err_unique_iface_field_impl_only.cb`, `err_unique_iface_field_iface_only.cb`;
  `testUniqueField` -> 43, `test_collection_leaks.cb` -> 112. Gates: `test.bat` Release all
  pass, `test_lsp.bat` 206/0, `example.bat` 90/0/27.

This made interface FIELDS better-enforced than interface METHODS - a separate pre-existing
bug, FIXED 2026-07-16: `VerifyInterfaceMethodContract` now checks param `IsMove` plus return
`IsMove`/`IsAlias`/`IsBond`. It forced a real `core/interfaces.cb` correction
(`IEnumerable`/`IList` were declaring plain returns for what `list<T>` hands back as `alias`
borrows) and exposed that **`IsAlias` was never in the `--init` round-trip** - now `"al"`,
alongside `"mv"`/`"bd"`/`"uq"`.

Related pre-existing fixes surfaced by the review, all closed and their issue files deleted:
alloc-align-clause-indirect-store (2026-07-16; note `ElementTypeIsOverAligned` carve-out for
type-carried alignment), deref-assign-owning-struct-double-free (2026-07-18; `*pc = *pa` now
auto-moves like `c = a`).

### Container FIELD migration (`list._data` etc. as `unique` fields) - REJECTED 2026-07-16

Recorded because "migrate the containers' own fields" is an obvious re-proposal. **Every
pointer field in `list`/`dictionary` is an ARRAY, not a single object** (`new T[cap]`, freed
by `delete[_size]`). `unique` emits a SINGLE-object delete - pointing it at `_data` would
destruct element 0, leak the rest, and free an array allocation through the scalar path. The
destructors need a runtime element count, an `if const` type branch, and array-delete
semantics; `~dictionary()` additionally does a sparse `_status[i] == 1` walk. What it would
take is a counted-array qualifier (`unique(_size) T* _data;`) - real machinery to delete ~28
lines at two call sites. If revived, spec it as its own design. (Element ownership - the
thing users actually care about - was instead solved by Part II's type-argument design.)

### Non-blocking cleanups (status: see live section item 5)

- Dead code: `GetOrCreateMemberwiseCopy`'s internal `TypeOwnsUniquePointer` bail
  (`LLVMBackend.h:3034-3036`) is unreachable - its only caller sits in the `else` of the same
  predicate. Not defensible as defence-in-depth: it returns `nullptr` silently, so if it ever
  fired the user would get `unknown function 'copy'` instead of the good message.
  (`return nullptr;` after a `LogError` is also dead - `LogError` is `[[noreturn]]`.)
- Trap A prints `unique field ''` for a bare self-field access in a method/constructor - the
  sibling diagnostics use `DescribeUniqueFieldOwner` / `SplitEnclosingStruct`; the Trap A one
  at `MainListener.h:8130-8132` was not given them.
- `unique int x[4]` reports a pointer message - the fixed-array check precedes the is-pointer
  check in `ValidateUniqueField`.
- `test_move.cb`: `unique_composes_with_user_dtor` and `unique_user_dtor_release_frees_once`
  are byte-identical; stale comment says the user dtor "frees and nulls" (it assigns null;
  Stage 2's reassignment path frees).
- `TypeOwnsUniquePointer` uses `std::set`, `UniqueChainReaches` uses `std::unordered_set`.
- On the `UniqueFreesItself` rewrite (`delete item; item = nullptr;` -> `item = nullptr;`):
  it DOES weaken the test, unavoidably - Stage 2 made hand-freeing a `unique` field illegal,
  so the original scenario is unrepresentable by design.

## Part I migration (the 34 fields) - UNBLOCKED, NOT STARTED

Opt-in, so nothing breaks. All blockers cleared 2026-07-17 (the last was the `(T*)` cast
hole, finding 2). The candidate set is the **34 verified-owning pointer fields** (of 254
scalar pointer fields across 144 structs). Each migration adds one keyword and **deletes** a
hand-written destructor.

Do not migrate by field name. The survey found name-based classification misfiles in both
directions: `JsonNode.next` (`json.cb:208`) and `XmlNode._nextSibling` (`xml.cb:211`) look
like classic borrows and are **owning** (`~JsonNode()` deletes the whole sibling chain) -
though both opt out anyway under D2/arena-duality; `ArenaAllocator._first_child` owns while
its adjacent `._next_sibling` borrows (`arena_allocator.cb:286-287`, same type, same struct).

Verification for the migration: `test.bat` (Release) + `test_lsp.bat` + `example.bat` green;
the HeapAudit leak oracle on each migrated type (the whole point is that the synthesized dtor
frees what the hand-written one used to); run the migration as its own commit so a regression
is attributable.

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

## Appendix: corrections owed to `ownership-move-alias-discipline.md`

Found while verifying Part I's design. That note is labelled "verified, not assumed" but:

1. **Its transfer table is mechanically wrong.** `h->slot = fresh;` does NOT transfer
   ownership. The nulling block (`MainListener.h:8227-8233`) explicitly excludes
   new-allocated locals escaping into a field; what fires instead is a lazy per-variable
   refcount (`:8190-8215`, init 1, bumped to 2 on escape) so scope exit decrements to 1 and
   declines to free (`EmitConditionalOwningPtrCleanup`, `LLVMBackend.h:1567-1577`). The end
   state is right, but `fresh` keeps `IsOwning`, is never marked moved, and a later
   `delete fresh;` is **undiagnosed** - a double-free the note does not mention.
2. **Both cited line ranges have drifted.** "cannot delete borrowed parameter" is at
   `MainListener.h:11753-11768`, not 11313; the call-site `move` guard is at
   `:11985-12002`, not 11553.
3. **`../issue/aligned-new-field-escape.md` is a dangling link** - fixed by the
   `alignas(slot, alloc)` work; `unified-alignas-slot-alloc.md:41` refers to it in the past
   tense.

Also: `doc/LANGUAGE.md:1075` stated fields own their pointers - true only for `unique`
fields since Stage 1.

---

# Part II - interface value ownership and `unique` in generic-argument position

Promoted 2026-07-18 from `internal/issue/list-interface-element-copy-imperfect.md` (deleted).
Promoted because the container symptom is not fixable in the container - it needs a
language-level answer to what an interface value owns.

## Problem

An interface value is a fat pointer `{ vtable*, data* }` (`__iface_fat_ptr`,
`LLVMBackend.h:7386`). Boxing never copies the object - `CoerceArgToInterface`
(`LLVMBackend.h:8087-8129`) stores the address of whatever storage already exists. So the
data pointer may point at a stack alloca or a heap block, and both are legal and both have
static type `IShape`:

```cflat
Triangle t;  IShape st = t;                 // data ptr -> STACK alloca
Circle* hc = new Circle(); IShape s = hc;   // data ptr -> HEAP block; boxing moves ownership
delete s;                                   // ...and delete through the interface is correct
```

`Test/test_interface.cb:95-106` exercises both.

**At scope level this is decidable.** The compiler sees the boxing site, and the existing
scalar `delete ifaceVar;` path (`MainListener.h:12522-12525` -> `DeleteInterfaceValue`,
`LLVMBackend.h:9613-9645`) already does the right thing: extract the data pointer, dispatch
the vtable dtor slot (`InterfaceDtorSlotIndex`, `LLVMBackend.h:9571`), free.

**Inside `list<T>` it is not.** Once the value is stored, the boxing site is gone; the fat
pointer records NOTHING about provenance. Both container answers are wrong: do not free ->
heap-boxed elements leak; free -> stack-boxed elements pass a stack address to
`operator delete` - heap corruption, strictly worse.

Pre-flip `list<T>` did the first, by accident: every ownership branch keyed off
`is_pointer(T)`, a text check for a trailing `*` (`MainListener.h:15591-15611`), false for
`"IShape"`; the value-type branches ALSO no-op because interfaces live in `interfaceTable`,
not `dataStructures`. The list freed its fat-pointer buffer and nothing else.

## Evidence

CONFIRMED 2026-07-18 (`scratch/iface_leak.cb`, `-o` build; HeapAudit needs a linked exe):

```cflat
HeapAudit.enable();
{
    list<IShape> shapes;
    int i = 0;
    while (i < 3) { Circle* c = new Circle(); c.r = 2; IShape s = c; shapes.add(s); i++; }
}
printf("leaks=%u\n", HeapAudit.reportLeaks());   // leaks=3
```

Independently corroborated by the interface-fields spike
(`internal/plan/interface-fields-feasibility.md:138-141`) - two separate investigations
reached this from different directions.

## Why the copy behaviour is NOT the bug

`list.cb`'s `else result.add(_data[i].copy())` branch resolves into the bitwise fallback
(`LLVMBackend.h:13930-13944`), which for an interface argument reloads the fat struct by
value - duplicating both pointers, cloning nothing. Downstream of non-ownership that is
arguably CORRECT: two lists of non-owning references aliasing the same objects is what
non-owning means. Under the shipped design, borrowed lists keep exactly this behaviour;
unique lists are move-only until a clone story exists (see "copy()").

## Candidates considered

1. **Provenance bit in the fat pointer.** Purely dynamic; even allows mixed owned/borrowed
   elements. Rejected as primary: a bitwise fat-pointer copy duplicates the owning bit ->
   double free, so it needs a "copies are borrowed, only moves carry the bit" rule - which is
   `unique` semantics enforced at runtime instead of compile time. Weaker guarantee, same
   rules, dynamic cost.
2. **Owning vs borrowed interface types (`unique IShape`).** CHOSEN, generalized to pointers.
3. **Restrict containers to explicit pointers (`list<IShape*>`).** Cheapest interim;
   superseded by the transitional diagnostic idea (below), which converts silent to loud the
   same way.
4. **Always-owning boxing (Go model).** Every boxing heap-allocates. Cleanest mental model,
   but breaks "boxing never copies", breaks stack boxing, and puts an allocation on every
   scoped upcast. Kept in the back pocket if the unique/borrowed split proves too ceremonial.
5. **Refcounted interface box.** Solves copy ergonomics but imports refcounting into a
   `unique`/`move` language - a philosophical fork. Rejected.

## Design: `unique` in generic-argument position

Motivating style constraint (user): interfaces exist to hold a value WITHOUT knowing the
concrete type. A "views only" doctrine forces owning positions back to concrete types,
defeating the point. Type erasure was never the obstacle - the vtable dtor slot already
erases the destructor; the only missing information is "do I own", one bit. Put it in the
type:

|             | borrowed (default)              | owning                                  |
|-------------|---------------------------------|-----------------------------------------|
| pointer     | `list<Circle*>` - never deletes | `list<unique Circle*>` - deletes each   |
| interface   | `list<IShape>` - never deletes  | `list<unique IShape>` - dtor slot + free|

This deliberately BROKE the old `list<T*>` owning-by-default, in exchange for pointers and
interfaces having the same story. It extends Part I's existing qualifier into a new position,
not a new concept.

### Mechanics

- **Parse + monomorphize.** `unique` legal inside a type argument, in BOTH
  `ParseDeclarationSpecifiers` copies; distinct instantiation with distinct mangling.
- **Substitution string carries the qualifier (decision).** The `activeTypeSubstitutions`
  entry for `T` is the full text `"unique Circle*"`, NOT a clean string plus a side flag.
  One source of truth; a consumer that does not understand the prefix fails loudly
  ("unknown type") instead of silently dropping the flag. Consequence: audit every consumer
  of the resolved string. `is_pointer` keeps working unmodified (string still ends in `*`).
- **`unique` IS part of T - but it qualifies storage locations, not rvalues (decision).**
  Three readings were weighed: (a) naive T = `unique Circle*` - propagation free, but every
  body mention of T would be owning (`return _data[i];` would move on every read - wrong);
  (b) T = `Circle*` plus a per-instantiation side flag - recreates the parallel-map problem,
  a second source of truth where a missed path is a SILENT wrong `is_unique`; (c) CHOSEN:
  T carries the qualifier, and the SEMANTIC rule fixes the read problem:
  - A declaration of type `unique X*` (local, param, field, buffer element) is an OWNING
    LOCATION.
  - Loading from an owning location WITHOUT `move` yields a borrowed `X*` rvalue - so
    `return _data[i];` returns a borrow even though the declared return type is T.
  - `return move _data[i];` transfers - exactly how `take()` was already written.
  - A parameter of unique type is a SYNTHESIZED move parameter (D4).
  Consequence for `list.cb`: `take()` and `get`/`operator[]` were correct AS WRITTEN; the API
  shape was accidentally designed for this. Known wrinkle: library-internal `T tmp =
  _data[i];` (e.g. swap in a sort) declares an owning local from a borrow - an error under
  this rule; such code must use `move` both ways, or a future qualifier-stripping operator
  (`borrow T`) covers scratch locals. Deferred.
- **`is_unique(T)` intrinsic.** Clone of `is_pointer`: test `resolved` starts with
  `"unique "`, return constant i1. Orthogonal to `is_pointer` (`unique Circle*` is both).
  Same footgun as the other intrinsics: returns i1 typed "int" - use under `if const` only,
  do not assert on printing it.
- **`list.cb` rewrite.** Ownership branches key off `is_unique(T)`. Teardown collapses to ONE
  branch, because scalar `delete` already dispatches raw-pointer vs interface by operand
  type: `if const (is_unique(T)) { if (_data[i] != nullptr) delete _data[i]; }`. The null
  skip covers slots nulled by `move l[i]` and stored nulls.
- **Element slots are `unique` fields in effect.** `_data[i] = item` on an owning list is an
  ownership transfer; the field-store rules (destruct-before-overwrite, alias reject) apply
  to buffer slots as to `unique` struct fields.
- **Boxing rule for `unique IShape`.** Only heap sources: `unique IShape s = new Circle();`
  is legal; boxing a stack value into a unique interface is a compile error (explicit `new`
  keeps allocation visible; no silent heap-promotion).
- **`--init` serializer.** Standing rule; no new bits were ultimately needed (reuses
  IsUnique/IsMove).

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
  needed F3 (interface null-compare). Resolved - see Stage 4: F3 was already on master.
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

### Prior art: C++ vector<unique_ptr<T>>

C++ answers the `[]` question with a reference: `operator[]` returns `unique_ptr<T>&`, and
the WRAPPER TYPE does all enforcement - the container is ownership-oblivious. The chosen
design maps one-to-one, and `list.cb` ALREADY had C++'s exact split:

- `unique_ptr<T>&` return  <->  `alias T get/operator[]` (`list.cb:52-59`, documented borrow)
- `v[i] = std::move(q)`    <->  `set(int, move T)` (already destructs-before-store)
- `std::move(v[i])` leaves empty ptr  <->  `move l[i]` leaves readable null; `take(i)` = the
  move-then-erase idiom, built in
- deleted copy ctor  <->  the storage-location rule (borrow into owning location = error) -
  same guarantee, enforced by the type system instead of a wrapper class

Shared footgun, shared honestly: the C++ reference dangles on push_back realloc; the CFlat
`alias` into `_data` has the identical invalidation-on-add hazard. Not a regression.

### Prior art: C++ by-value sink params (added 2026-07-19, for the D12 value leg)

The `vector<unique_ptr<T>>` section above covers the unique leg. The VALUE leg has its own
C++ answer, and it bears directly on unblock options (a)/(b)/(c):

- **Plain by-value params OWN in C++, unconditionally.** `void f(std::string s)` does not
  shallow-share; the param is CONSTRUCTED from the argument - copy-constructed from an
  lvalue (independent buffer), move-constructed from an rvalue (steals it, source left in a
  valid moved-from state) - and destructed at scope exit either way. cflat's shallow-share
  has no C++ equivalent for a by-value param; the nearest thing is `const T&`, which is
  exactly the case where a C++ programmer knows they must copy before storing. So Blocker 1
  cannot arise in C++.
- **Value category carries the call-site fact.** `std::move(x)` is not a runtime operation -
  it is `static_cast<T&&>(x)`, a pure type-level cast. It changes the expression's TYPE,
  which is why overload resolution can see it. That is the channel cflat lacks for value
  types (Blocker 2).
- **The insertion idiom is the by-value sink param**: `void set(std::string s) { field_ =
  std::move(s); }`. ONE signature, both behaviors, chosen at the call site - lvalue =>
  copy-ctor then move into place; `std::move(x)` => move-ctor then move into place. This is
  precisely what D12's plain-`T` insertion param is trying to be, and it works BECAUSE
  by-value params own. `vector::push_back` uses the two-overload `const T&`/`T&&` form
  instead, but that is an optimization (saves one move), not a semantic necessity;
  `emplace_back` + perfect forwarding is a third answer.
- **cflat-specific wrinkle for option (b):** C++ leans on per-type copy/move CONSTRUCTORS.
  cflat has no such hooks - it has `string`'s runtime owned bit, auto-move for owning
  structs, `.copy()`, and `ClosureCaptureDeepCopyable`. Option (b) would need a defined
  "deep-copy a named lvalue into the param" answer per value kind, wired into param binding.
  Those mechanisms exist; they are not connected to parameter passing today.

Considered alternative from this comparison: a library `box<T>` wrapper (dtor deletes,
move-only) would make `list<box<IShape>>` work with ZERO compiler container knowledge.
Rejected: it requires the missing primitive anyway (a way to delete copies of a struct -
`unique` storage IS that primitive, expressed as a qualifier), costs unwrapping ergonomics at
every use, and the fat-pointer interface case fits the qualifier more naturally. The chosen
design is unique_ptr inlined into the type system.

### copy()

Bitwise copy of unique elements is a double-free. Unique instantiations are MOVE-ONLY at
first: `copy()` on `list<unique T*>` / `list<unique IShape>` is a compile error, via the
`compile_error` builtin (see Stage 5 for the deferred-poison mechanism). A later clone story
for interfaces needs a vtable copy slot every implementor fills; for pointers, an element
`.copy()` through `new`. Not in scope.

### Migration (type arguments) - DONE in Stage 5

Inventory (grep 2026-07-18): 55 `list<T*>` sites across `cflat/core`, `Test`, `example`
(~30 distinct declarations). Sites that DOCUMENTED reliance on owning-by-default became the
regression suite: `ui_test.cb:345`, `numa.cb:170`, the `_windows` lists in
`ui_native/win32.cb` + `cocoa.cb`, `test_move.cb` recursive `list<TreeBox*>` teardown,
`test_core.cb:639-661` take/removeAt dtor-count assertions, `test_program.cb`,
`test_generics.cb`.

The transitional diagnostic idea (bare `list<T*>` = compile error during the migration
window) was resolved as: no transitional diagnostic; the classification table + dtor-count
oracles were the net.

## Interaction with interface fields (F1)

`internal/plan/interface-fields-feasibility.md` F1 makes interfaces more value-like. Layering
rule that keeps the two plans independent:

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

## Part II stages (implementation started 2026-07-18, on master; breaking change)

1. DONE 2026-07-18 (test.sh Release 405/0/8). `unique` parses in type-argument position;
   canonical `"unique X*"` substitution text; distinct instantiation; D1 diagnostic.
   Mangling: `list<unique Circle*>` -> `list__unique_Circleptr`. Grammar:
   `typeParameterEntry` takes an optional leading Identifier, validated as `unique` in the
   listener (keeps the soft keyword; no lexer token). Chokepoint `PeelTypeArgSuffix` strips
   the prefix for LLVM-type resolution; manual consumers touched:
   `ResolveSigComponentCodegen`, `GenerateDefaultValue`, qualified-name helper (~:11237).
   list.cb untouched at this stage.
2. DONE except `compile_error`: `is_unique(T)` landed with stage 1; `compile_error` moved to
   stage 5 where first needed.
3. DONE 2026-07-18 (test.sh Release 407/0/8). Semantics for the DIRECT `unique X*` spelling
   on locals and params:
   - Synthesized move params (D4): both `ParseParameterTypeList` copies set IsMove when the
     param type is unique (single-indirection pointer or interface). Reuses the whole
     existing move-param machinery; unique-to-plain-param is automatically a borrow.
   - D5 decl-init rule: unique local must initialize from `new` / `move` / move-returning
     call / `nullptr`; borrowed source = error. `delete` on a unique local rejected (assign
     nullptr to free); message wording says "unique field" - polish later.
   - Gap hardening, corrected findings: tuple and using-alias were NOT silently colliding
     (both mangle distinctly). using-alias of `list<unique X*>` works. Tuple element position
     now REJECTS unique (tuple has no dtor - an owning element would leak). The
     generic-function-call getText paths (both copies) were genuinely broken and now reject
     unique with a clear message.
   - Stable error strings (used in Test/errors/): "unique requires a pointer or interface
     type; '<T>' is neither"; "cannot initialize unique '<name>' from a borrowed value - the
     source still owns it; use 'new', a 'move' expression, or a move-returning call, or drop
     'unique'"; "unique is not supported as an explicit generic function type argument";
     "unique is not supported as a tuple element type".
   - Tests: test_move.cb testUniqueLocalAndParam; new err_unique_non_pointer_local /
     err_unique_borrow_into_unique / err_unique_rejected_position. DELETED
     err_unique_not_a_field.cb and err_unique_param.cb (asserted the pre-D1 field-only
     restriction this design overturns).
   - SCOPING DEVIATION (deliberate): post-substitution `unique` (a generic T bound to
     "unique X*") did NOT get D4/D5 semantics until the stage-5 flip - synthesis earlier
     would have broken list.cb internals and turned borrow-params like `hashset.contains(T)`
     into move params.
4. DONE 2026-07-18: F3 (interface null-compare) was ALREADY ON MASTER -
   `LowerInterfaceNullCompare` (MainListener.h:9258-9272, hooked at :9298) landed in commit
   db1624e with the interface-fields work; the earlier "spike-only" claim was wrong. Coverage
   added to Test/test_interface.cb (default-init / stack-boxed / heap-boxed, both operators,
   both operand orders; 35/35). REMAINING GAP found by boundary check, moved into stage 5:
   stack boxing into a unique interface local was not rejected. (Closed in stage 5.)
5. DONE 2026-07-18 (test.sh Release 411/0/8; example_mac.sh 35/0; warm --init cache
   re-verified; the Evidence repro rewritten as `list<unique IShape>` runs HeapAudit-clean,
   leaks=0). THE FLIP landed in one change:
   - `list.cb` ownership branches (dtor/set/removeAt/clear/copy) key off `is_unique(T)`.
     Bare `list<X*>` / `list<IFace>` is BORROWED (never frees); owning is
     `list<unique X*>` / `list<unique IShape>`.
   - `compile_error` builtin, DEFERRED-POISON mechanism: generic methods instantiate eagerly,
     so fire-on-instantiation would reject mere declaration. Instead it records the enclosing
     function in `poisonedFunctions`; `CheckPoisonedFunctionCalls()` errors only if a real
     CallBase user exists (vtable-slot references do not count). copy() on a unique list =
     "cannot copy a list of unique elements; use move or copy elements explicitly". KNOWN
     GAP: a poisoned method reached ONLY via virtual interface dispatch is not caught (no
     such call in-repo).
   - Stage-4 boxing gap fixed: stack boxing into `unique IShape` local rejected (D5 message);
     heap `new` legal. err_unique_iface_stack_box.cb.
   - Interface container leg: `list<unique IShape>` frees via the vtable dtor slot - the
     branch that had never executed anywhere, now dtor-count + HeapAudit asserted in
     Test/test_collection_leaks.cb. Bare `list<IShape>` verified non-destroying.
   - Migration: per-site classification table in the stage-5 agent report (~30 declarations
     across core/Test/example; owning -> unique, genuinely-borrowed stayed bare, e.g.
     ui_native.cb IElement lists owned by the reconciler tree, and one intentional
     borrowed-coexist list in test_generics). Windows-bound files (ui_native/win32.cb,
     example fsh.cb, todo_api.cb) migrated syntactically, not exercisable on macOS.
   - OPEN WART found here - see live section item 2. Decide before stage 6 spreads the
     pattern (stage 6 was instructed to mirror, not decide).
6. DONE 2026-07-19 (test.sh Release 419/0/8 cold AND warm cache; example_mac.sh 35/0).
   `dictionary` / `hashset` same treatment. **No compiler changes needed** - `is_unique`,
   unique type-arg parsing/mangling, synthesized move params, and the deferred
   `compile_error` poison covered everything.
   - `dictionary.cb`: all VALUE-ownership branches switched `is_pointer(V)` ->
     `is_unique(V)`, mirroring list.cb (unique branch with null-skip, `else if const
     (!is_pointer(V))` value-type branch): `~dictionary()` (sparse `_status[i]==1` walk
     preserved), `clear()`, `remove()`, and the `set()` overwrite path
     (destruct-before-overwrite). `copy()` gains the `compile_error` poison when V is
     unique; the bare-pointer value branch changed from `= move _values[i]` to a plain
     shallow alias (borrowed = shared, not transferred). `dictionary<K, unique IFace>`
     frees via the vtable dtor slot; bare interface values borrow.
   - **Unique KEYS rejected** (and hashset elements, which are key-like): keys are read by
     `_slot()` for hashing/equality on EVERY operation, so a unique K would destructively
     move the caller's key argument out on each lookup, and stored keys would leak (they
     hit the store-as-is pointer branch). A `compile_error` gated on
     `if const (is_unique(K))` sits on the shared `_slot()` path in both containers, so any
     real operation on a unique-key instantiation is rejected. Hashset otherwise needed no
     ownership change - it already had the correct borrowed contract.
   - `_missing` note: when V is unique, `dictionary`'s `V _missing` scalar field becomes a
     synthesized-null-guarded unique field; it is only ever default-null, so teardown is a
     verified no-op (leak oracle).
   - Migration classification (all core/example dictionaries hold primitive/string values -
     unaffected): `test_generics.cb:816` `dictionary<int, Employee*>` and `:2503`
     `dictionary<int, list<int>*>` were owning -> `unique`; `:777` `hashset<Employee*>`
     borrowed (manual deletes) -> left bare; `test_collection_leaks.cb:931`
     `dictionary<int, int*>` reworked to borrowed shallow-alias copy semantics. `hpc/
     btree.cb` still uses the old `is_pointer` value contract independently; its "mirrors
     dictionary exactly" comment is now slightly stale (out of scope, noted in polish debt).
   - Tests: `test_collection_leaks.cb` 170 -> 176 assertions (new Stage 6 section: unique-
     ptr dict frees at remove/overwrite/clear/dtor; bare-ptr dict frees nothing; unique-
     interface dict frees via vtable; hashset<ptr> frees nothing); `test_generics.cb` still
     72/72; new negatives `err_unique_dict_copy.cb`, `err_unique_dict_key.cb`,
     `err_unique_hashset_elem.cb`.
7. **D12 implementation - ATTEMPTED AND ROLLED BACK 2026-07-19. BLOCKED, not done.**
   See live-section item 2 for the premise error and the two blockers. Verification of the
   rolled-back tree: test.sh Release 419/0/8 (run twice, cold + warm cache);
   example_mac.sh 35/0 - i.e. back to the Stage 6 baseline.

   **What landed (the only piece kept):** `dictionary` and `hashset` declare their
   key/element query params `alias` - `_slot`, `contains`, `get`, `remove`, `operator[]`,
   and the KEY parameter of `add`/`set` (dictionary), plus `_slot`/`add`/`contains`/`remove`
   (hashset). This is the borrow contract those params always had implicitly: a lookup reads
   the key for hashing/equality and must never consume it. Inert today (a unique key/element
   is rejected outright), but it is the prerequisite that keeps query params borrows once
   insertion params ever become plain `T`. Suite-verified green at 419/0/8.

   **Second spike, 2026-07-19 (validation only, reverted).** Re-derived the withheld D4
   synthesis and proved both POINTER legs correct; disproved the two-pass blocker; pinned
   the real failure to the owning-value leg. Full findings in live-section item 2. The
   patch is preserved at `scratch/d12-pointer-leg-spike.patch` (`MainListener.h` +
   `list.cb` `add`/`set` plain-`T` + the matching `IList<T>` contract edit in
   `interfaces.cb` - `VerifyInterfaceMethodContract` compares param `IsMove`, so the
   interface must move in step). Re-appliable with `git apply` once the value leg is
   unblocked. Tree reverted and re-verified green: test.sh 419/0/8, example_mac.sh 35/0.

   **Reference behaviour recorded while spiking (a struct with a `unique` field):**
   synthesized `.copy()` is REJECTED, with the diagnostic naming the field and both fixes
   ("write a `copy()` ... or `move` the value"); assignment `Holder c = a;` AUTO-MOVES and
   a later read of `a` is `use of moved variable 'a'`; a user-written `copy()` takes
   precedence and works; and the rejection is TRANSITIVE through by-value members, naming
   the full path (`Nested.h.p`). Note the struct does NOT itself become `unique`:
   `is_unique(T)` is purely syntactic (`MainListener.h:15894`, leading `"unique "` in the
   substitution string), while ownership of the TYPE is the separate transitive
   `TypeOwnsUniquePointer` (`LLVMBackend.h:3145`), which recurses through by-value members
   and deliberately STOPS at pointer members ("a pointer member is a borrow the copy
   shallow-shares by design"). Consequence for containers: `list<Holder>` takes the
   value-type path, not the unique path - so it is on the blocked value leg, not the
   proven pointer leg.

   **What was built, verified in isolation, and deliberately NOT landed:** post-substitution
   D4 unique synthesis - lifting the Stage 3 scoping deviation so a param declared plain `T`
   becomes a synthesized move param when T binds to `"unique X*"`. Implemented as a scoped
   `inParamDecl_` guard around the codegen `ParseParameterTypeList`'s specifier parse, with
   the substitution branch of `ParseDeclarationSpecifiers` capturing the peeled `unique`
   prefix (and skipping the promotion when the param is `alias`, so query params stay
   borrows). Proven working by spike (a `Box<unique Thing*>.put(T)` argument transferred and
   the source went use-after-move; an `alias T` param stayed a borrow). NOT landed for two
   reasons: (i) with insertion params back on `move T value` it has no consumer, and
   (ii) it regressed `test_collection_leaks` (abort), attributed at the time to a two-pass
   `IsMove` disagreement.
   **That attribution was WRONG and is retracted (2026-07-19, second spike).** The scanner
   has no `activeTypeSubstitutions` member at all and skips generic templates outright, so
   the disagreement was structurally impossible; the regression was the owning-VALUE leg
   (Blocker 1) all along. See live-section item 2. Do NOT carry "solve two-pass agreement
   first" into a future attempt - it is a false blocker.

   **Audit result (kept - it is the reusable part of the analysis).** Every T/K/V-typed
   parameter across the three containers, classified insertion vs query:
   - `list`: `add(T)`, `set(int, T)` are the ONLY value-typed params, both INSERTION. There
     is no query-by-value method (no `contains`/`indexOf`), so list needs no borrow spelling.
     The `Lambda<bool(T,T)>` comparators on `_partition`/`_medianPivot`/`_qsort`/`sort` are
     closure params, not element params, and are unaffected.
   - `dictionary`: the VALUE param of `add`/`set` is INSERTION; every K param
     (`_slot`, `add`/`set` key, `contains`, `get`, `remove`, `operator[]`) is QUERY -> `alias`.
   - `hashset`: elements are key-like, so `_slot`, `add`, `contains`, `remove` are ALL
     QUERY -> `alias`. `add` was already plain `T` and needed no signature change.
   Note this means no LEGAL unique instantiation ever puts a unique value in a query param:
   list has no query-by-value, and unique dictionary keys / hashset elements are rejected.
   The `alias` spelling matters only to keep the rejection diagnostic clean (without it the
   synthesis turns `_slot`'s key into a move param and `_rehash` reports "use of moved
   variable 'oldKeys'" instead of the intended "dictionary keys cannot be 'unique'").

   **Also confirmed while here:** `IList<T>` in `core/interfaces.cb` declares
   `add(move T)` / `set(int, move T)`, and `VerifyInterfaceMethodContract` compares param
   `IsMove`, so flipping `list`'s insertion params to plain `T` forces a matching
   `core/interfaces.cb` edit. `IDictionary<K,V>` already declares plain `V` (dictionary does
   not implement it, so no contract check fires). `IQueue.push/send` keep `move` - a channel
   genuinely hands off ownership and is out of D12's scope.

## Related

- `internal/plan/ownership-move-alias-discipline.md` - parameter discipline (see Part I
  Appendix for corrections owed to it)
- `internal/plan/interface-fields-feasibility.md` - F1; see "Interaction with interface
  fields"
- `internal/plan/ownership-sanitizer.md`, `internal/plan/move-dataflow.md` - runtime and
  dataflow companions to the static rules here
- `internal/issue/brace-init-field-store-not-at-parity.md` - the durable store-path-parity
  rule (shared helpers from the start), learned partly from this feature
- `Test/test_interface.cb:95-106` - both boxing forms
- `Test/test_core.cb:639-661` - the owning-list API contract
- `Test/test_collection_leaks.cb` - leak regression oracles (HeapAudit + dtor counts)
