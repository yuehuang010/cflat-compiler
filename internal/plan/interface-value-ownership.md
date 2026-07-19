# Interface value ownership: provenance beyond the scope

Promoted 2026-07-18 from `internal/issue/list-interface-element-copy-imperfect.md` (deleted;
this file supersedes it). Promoted because the container symptom is not fixable in the
container - it needs a language-level answer to what an interface value owns.

Updated 2026-07-18 after design discussion: direction chosen (see "Proposed design"), not
yet staged for implementation.

## Problem

An interface value is a fat pointer `{ vtable*, data* }` (`__iface_fat_ptr`,
`LLVMBackend.h:7386`). Boxing never copies the object - `CoerceArgToInterface`
(`LLVMBackend.h:8087-8129`) stores the address of whatever storage already exists. So the data
pointer may point at a stack alloca or a heap block, and both are legal and both have static
type `IShape`:

```cflat
Triangle t;  IShape st = t;                 // data ptr -> STACK alloca
Circle* hc = new Circle(); IShape s = hc;   // data ptr -> HEAP block; boxing moves ownership
delete s;                                   // ...and delete through the interface is correct
```

`Test/test_interface.cb:95-106` exercises both.

**At scope level this is decidable.** The compiler sees the boxing site, knows whether the
source was a local or a `new`, and the existing scalar `delete ifaceVar;` path
(`MainListener.h:12522-12525` -> `DeleteInterfaceValue`, `LLVMBackend.h:9613-9645`) already
does the right thing: extract the data pointer, dispatch the vtable dtor slot
(`InterfaceDtorSlotIndex`, `LLVMBackend.h:9571`), free.

**Inside `list<T>` it is not.** Once the value is stored in a container, the boxing site is
gone; only the fat pointer survives, and it records NOTHING about provenance. The container
cannot decide whether to free. Both answers are wrong:

- Do not free -> heap-boxed elements leak.
- Free -> stack-boxed elements pass a stack address to `operator delete`. Heap corruption,
  strictly worse.

Today `list<T>` does the first, by accident rather than decision: every ownership branch keys
off `is_pointer(T)`, a text check for a trailing `*` (`MainListener.h:15591-15611`), which is
false for `"IShape"`. It then falls into the value-type branches, which ALSO no-op, because
`.~()` resolves through `GetOrCreateFullDestructor` (`LLVMBackend.h:2913-2915`) and the array
delete gates on `IsDataStructure` (`MainListener.h:12582`) - both search `dataStructures`, and
interfaces live in `interfaceTable`. The list frees its fat-pointer buffer and nothing else.

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

Three `*** cflat heap-audit: LEAK ***` traces, each pointing at the `new Circle()`.

Independently corroborated: the interface-fields spike hit the same leak and scoped it out -
`internal/plan/interface-fields-feasibility.md:138-141` records "ONE 64-byte leak... a
`list<IFace>` with one `add` and no interface fields anywhere leaks the same 64 bytes." Two
separate investigations reached this from different directions.

## Why the copy behaviour is NOT the bug

`list.cb`'s `else result.add(_data[i].copy())` branch resolves into the bitwise fallback
(`LLVMBackend.h:13930-13944`), which for an interface argument reloads the fat struct by value -
duplicating both pointers, cloning nothing. Downstream of non-ownership that is arguably
CORRECT: two lists of non-owning references aliasing the same objects is what non-owning means.
Do not fix the copy in isolation. Under the proposed design, borrowed lists keep exactly this
behaviour; unique lists become move-only until a clone story exists (see "copy()").

## Candidates considered

1. **Provenance bit in the fat pointer.** Purely dynamic - widen the fat pointer or steal a
   low bit of the vtable pointer; boxing from `new` sets it. Preserves type erasure with no
   syntax, and even allows mixed owned/borrowed elements in one list. Rejected as primary:
   a bitwise fat-pointer copy duplicates the owning bit -> double free, so it needs a
   "copies are borrowed, only moves carry the bit" rule - which is `unique` semantics
   enforced at runtime instead of compile time. Weaker guarantee, same rules, dynamic cost.
2. **Owning vs borrowed interface types (`unique IShape`).** CHOSEN, generalized to pointers -
   see below.
3. **Restrict containers to explicit pointers (`list<IShape*>`).** Cheapest interim; converts
   the silent leak into a compile error but costs expressiveness. Superseded: the transitional
   diagnostic in the migration plan below provides the same silent-to-loud conversion.
4. **Always-owning boxing (Go model).** Every boxing heap-allocates; interface values are
   always owning handles. Cleanest mental model, but breaks "boxing never copies", breaks
   `Test/test_interface.cb` stack boxing, and puts an allocation on every scoped upcast.
   Kept in the back pocket if the unique/borrowed split proves too ceremonial in practice.
5. **Refcounted interface box.** Copies share, last drop frees. Solves copy ergonomics but
   imports refcounting into a `unique`/`move` language - a philosophical fork. Rejected.

## Proposed design: `unique` in generic-argument position

Motivating style constraint (user): interfaces exist to hold a value WITHOUT knowing the
concrete type. A "views only" doctrine forces owning positions (containers, fields,
long-lived state) back to concrete types, defeating the point. Type erasure was never the
obstacle - the vtable dtor slot already erases the destructor; the only missing information
is "do I own", which is one bit. Put that bit in the type:

|             | borrowed (default)              | owning                                  |
|-------------|---------------------------------|-----------------------------------------|
| pointer     | `list<Circle*>` - never deletes | `list<unique Circle*>` - deletes each   |
| interface   | `list<IShape>` - never deletes  | `list<unique IShape>` - dtor slot + free|

This deliberately BREAKS today's `list<T*>` owning-by-default, in exchange for pointers and
interfaces having the same story. `unique` already exists as a field qualifier with dtor
synthesis (`LLVMBackend.h:2932`) and an interface-field contract check
(`VerifyInterfaceFields`, `LLVMBackend.h:7757`) - this extends an existing qualifier into
generic-argument position, not a new concept.

### Mechanics

- **Parse + monomorphize.** `unique` legal inside a type argument, in BOTH
  `ParseDeclarationSpecifiers` copies. `list<unique Circle*>` is a distinct instantiation
  from `list<Circle*>` with distinct mangling (e.g. `list__unique_Circle_ptr`).
- **Substitution string carries the qualifier (decision).** The `activeTypeSubstitutions`
  entry for `T` is the full text `"unique Circle*"`, NOT a clean string plus a side flag.
  One source of truth; a consumer that does not understand the prefix fails loudly
  ("unknown type") instead of silently dropping the flag. Consequence: audit every consumer
  of the resolved string (`GetType`, mangling, both ParseDeclarationSpecifiers copies on
  re-parse of substituted types, `_data` field registration) to strip or honor the prefix.
  `is_pointer` keeps working unmodified (string still ends in `*`).
- **`unique` IS part of T - but it qualifies storage locations, not rvalues (decision).**
  The question "is unique inside the template parameter" has three readings:
  (a) T = `unique Circle*` naively: propagation/mangling free, but then EVERY body mention
  of T is owning - `T operator[](int i) { return _data[i]; }` would move ownership out on
  every read. Wrong. (b) T = `Circle*` plus a per-instantiation side flag: body stays clean,
  but this recreates the parallel-map problem - a second source of truth both passes must
  propagate through every nested instantiation, where a missed path is a SILENT wrong
  `is_unique` answer; and `is_unique(T)` stops being a property of T. (c) CHOSEN: T carries
  the qualifier (keeps (a)'s plumbing - one substitution string, free propagation through
  nested generics, honest `is_unique`), and the SEMANTIC rule fixes the read problem:
  - A declaration of type `unique X*` (local, param, field, buffer element) is an OWNING
    LOCATION.
  - Loading from an owning location WITHOUT `move` yields a borrowed `X*` rvalue - so
    `return _data[i];` returns a borrow even though the declared return type is T.
  - `return move _data[i];` transfers - exactly how `take()` is already written.
  - A parameter of unique type is a SYNTHESIZED move parameter (ruled 2026-07-18):
    ownership transfer is declared by the callee, not the call site. `shapes.add(c);`
    transfers and nulls `c` - no call-site `move`. You cannot pass ownership to something
    that does not expect it: a unique value passed to a plain-pointer parameter is a
    borrow; ownership stays with the caller.
  Consequence for `list.cb`: `take()` and `get`/`operator[]` are correct AS WRITTEN;
  `add(move T)`'s explicit `move` becomes redundant when T is unique (synthesized) and
  may be removed; only the dtor/copy branches change. The API
  shape was accidentally designed for this. Known wrinkle: library-internal
  `T tmp = _data[i];` (e.g. swap in a sort) declares an owning local from a borrow - an
  error under this rule; such code must use `move` both ways, or a future
  qualifier-stripping type operator (`borrow T`) covers scratch locals. Deferred - current
  list.cb barely has such patterns.
- **`is_unique(T)` intrinsic.** Clone of `is_pointer` (`MainListener.h:15591-15612`): look up
  `T` in `activeTypeSubstitutions`, test `resolved` starts with `"unique "`, return constant
  i1. Orthogonal to `is_pointer` (`unique Circle*` is both). Same footgun as the other
  intrinsics: returns i1 typed "int", so printf'ing it directly shows garbage - use under
  `if const` only, do not assert on printing it.
- **`list.cb` rewrite.** Ownership branches key off `is_unique(T)` instead of the
  `is_pointer(T)` guess. Teardown collapses to ONE branch, because scalar `delete`
  already dispatches raw-pointer vs interface by the operand's type:
  `if const (is_unique(T)) { if (_data[i] != nullptr) delete _data[i]; }`.
  The null skip covers slots nulled by `move l[i]` and stored nulls. DEPENDENCY (D8):
  for interface T the `!= nullptr` compare does not compile on master - the fat struct
  reaches `CreateOperation` as an aggregate. The fix is F3 (interface null-compare,
  data-pointer icmp), proven in the interface-fields spike but NOT landed. Sequence:
  pointer leg first (unblocked today), then F3, then the interface leg. The interface
  delete branch has never executed anywhere - write a dtor-count test for it specifically.
- **Element slots are `unique` fields in effect.** `_data[i] = item` on an owning list is an
  ownership transfer; the field-store rules (destruct-before-overwrite, alias reject) must
  apply to buffer slots as they do to `unique` struct fields.
- **Synthesized move parameters (ruled 2026-07-18).** A parameter of unique type is
  implicitly a move parameter: the callee's signature declares the ownership sink, the
  call site stays `shapes.add(c)`, and the argument is nulled after the call. Passing a
  unique value to a non-unique parameter is a borrow. (Supersedes two earlier ideas:
  blanket implicit move-on-pass AND required call-site `move`.) `operator[]` returns a
  borrow; `take(i)` returns a move - the API `Test/test_core.cb:639-661` already asserts.
- **Boxing rule for `unique IShape`.** Only heap sources: `unique IShape s = new Circle();`
  is legal; boxing a stack value into a unique interface is a compile error (explicit `new`
  keeps allocation visible; no silent heap-promotion).
- **`--init` serializer.** Any new bit on `TypeAndValue` / instantiation records must be
  added to the `LLVMBackend.cpp` cache round-trip in the same change (see CLAUDE.md testing
  rule), or `expect_error` tests silently stop firing on a warm cache.

### Settled decisions (ballot, ruled 2026-07-18)

- **D1 - positions.** `unique` legal in any declaration position (locals, fields - already
  exists - and type arguments), only on pointer or interface types. `unique int` /
  `unique Circle` (value struct) = compile error "unique requires a pointer or interface
  type".
- **D2 - whole-type qualifier (user ruling).** `unique` applies to the ENTIRE declared
  pointer type - `unique Circle**` is one owning value that is, at the end, still a
  pointer. No per-star binding; owning-inner-pointer distinctions are unspellable and
  unneeded. Buffer internals do not depend on parsing the concatenated string: `_data[i]`
  is typed T (which carries unique) by indexing, regardless of how `T* _data` reads as
  flat text. `is_unique` = leading `"unique "`, `is_pointer` = trailing `*`; both hold on
  `unique Circle**`.
- **D3 - signatures (amended after stage 3).** A `unique`-typed PARAM is legal and is
  exactly a synthesized move param (D4) - the two spellings are defined equivalent, so
  they cannot drift. `unique` in RETURN position stays unsupported; transfer out is
  `move`, borrowing out is `alias`. (Original D3 forbade params too; D4's synthesis rule
  superseded that, and the two err_ tests asserting the old field-only restriction were
  deleted in stage 3.)
- **D4 - synthesized move params (user ruling).** See bullet above. Callee declares the
  ownership sink; no call-site `move`; unique-to-non-unique pass is a borrow.
- **D5 - borrows and moves through alias.** Bare local from a borrow is borrowed (no
  IsOwning). `unique X* u = l[i];` without move = error. `move l[i]` is allowed: nulls the
  slot in place, size unchanged (the std::move(v[i]) idiom); `take()` is remove-and-transfer.
- **D6 - no conversion** between `list<unique T*>` and `list<T*>`; distinct instantiations,
  overloads do not cross. Borrowed views of owning lists = future work.
- **D7 - `is_unique` outside a generic context** returns 0, same convention as `is_pointer`
  (both consult only `activeTypeSubstitutions`).
- **D8 - teardown + F3 dependency.** Single dtor branch with null skip; interface leg
  blocked on F3 (see the list.cb rewrite bullet). Pointer leg unblocked today.
- **D9 - `delete` on a borrowed raw pointer** stays legal (C compat); double-free is
  `--sanitize` territory, no static diagnostic.
- **D10 - canonical text.** One normalization point yields `"unique Circle*"` (single
  space, attached star); substitution map and mangling derive from it.
- **D11 - serializer.** Any new `TypeAndValue`/`StructData` bit joins the `--init`
  round-trip in the same change (standing repo rule).

### Prior art: C++ vector<unique_ptr<T>>

C++ answers the `[]` question with a reference: `operator[]` returns `unique_ptr<T>&`, and
the WRAPPER TYPE does all enforcement - the container is ownership-oblivious. Read borrows
through the reference; `auto p = v[i];` is a compile error (deleted copy ctor);
`auto p = std::move(v[i]);` leaves an empty unique_ptr in the slot; `v[i] = std::move(q);`
deletes the old pointee first via `operator=(&&)`; copying the vector is implicitly deleted.

The chosen design maps one-to-one, and `list.cb` ALREADY has C++'s exact split:

- `unique_ptr<T>&` return  <->  `alias T get/operator[]` (`list.cb:52-59`, documented borrow)
- `v[i] = std::move(q)`    <->  `set(int, move T)` (`list.cb:61-74`, already
  destructs-before-store; only the branch generalizes is_pointer -> is_unique + iface delete)
- `std::move(v[i])` leaves empty ptr  <->  `move l[i]` leaves readable null (CFlat's
  move-nulls-source matches exactly); `take(i)` = the move-then-erase idiom, built in
- deleted copy ctor  <->  the storage-location rule (borrow into owning location = error) -
  same guarantee, enforced by the type system instead of a wrapper class

Shared footgun, shared honestly: the C++ reference dangles on push_back realloc; the CFlat
`alias` into `_data` has the identical invalidation-on-add hazard. Not a regression.

Considered alternative from this comparison: a library `box<T>` wrapper (dtor deletes,
move-only) would make `list<box<IShape>>` work with ZERO compiler container knowledge -
C++'s architecture. Rejected: it requires the missing primitive anyway (a way to delete
copies of a struct - `unique` storage IS that primitive, expressed as a qualifier), costs
unwrapping ergonomics at every use, and the fat-pointer interface case fits the qualifier
more naturally. The chosen design is unique_ptr inlined into the type system.

### copy()

Bitwise copy of unique elements is a double-free. Unique instantiations are MOVE-ONLY at
first: `copy()` on `list<unique T*>` / `list<unique IShape>` is a compile error. This exposes
a missing mechanism - library code cannot currently mark a method as a compile error for a
specific instantiation. Smallest addition: a `compile_error("...")` builtin usable inside
`if const`. It will be wanted again for `dictionary<K, unique V*>`. A later clone story for
interfaces needs a vtable copy slot every implementor fills; for pointers, an element
`.copy()` through `new`. Not in scope for the first landing.

### Migration

Inventory (grep 2026-07-18): 55 `list<T*>` sites across `cflat/core`, `Test`, `example`
(~30 distinct declarations). Sites that DOCUMENT reliance on owning-by-default and therefore
must gain `unique` and become the regression suite:

- `cflat/core/ui_test.cb:345` (comment: "owns its elements")
- `cflat/core/numa.cb:170` (comment: "domain owns them")
- `cflat/core/ui_native/win32.cb:3050` + `:4873`, `cocoa.cb:1609` + `:3477` (`_windows`
  lists torn down by move)
- `Test/test_move.cb:359-383` (recursive `list<TreeBox*>` teardown)
- `Test/test_core.cb:639-661` (`take()`/`removeAt()` ownership + dtor-count assertions)
- `Test/test_program.cb:77-83`, `Test/test_generics.cb` pointer-element cases

**Transitional diagnostic (silent-flip guard).** An unmigrated `list<T*>` silently flips
from owning to leaking - the same silent-failure class this plan exists to kill. For the
migration window, bare `list<T*>` (pointer or interface type argument, no qualifier) is a
compile error: "element ownership is now explicit: write list<unique T*> (owning) or mark
the borrow". Every silent flip becomes loud; delete the diagnostic once the repo is green.
Open sub-question: whether the borrowed spelling during the window is bare-after-ack, a
`borrowed` soft keyword, or the diagnostic is simply temporary and bare = borrowed after it.

## Interaction with interface fields (F1)

`internal/plan/interface-fields-feasibility.md` F1 makes interfaces more value-like (fields,
lvalues, owned strings inside). Layering rule that keeps the two plans independent:

- **Fields are owned by the implementor object, or by a `unique` interface value that owns
  the object - never by a borrowed view.** All field-store rules apply through the interface
  path via the `IsInterfaceField` flag (flag-based, never GEP-shape-based - the spike's
  hardest-won lesson), on BOTH store paths (assignment and `EmitOneFieldInit` brace-init).
- **Teardown is free.** The vtable fullDtor is the implementor's dtor, which already destroys
  its owned fields. Whichever container knows to invoke the dtor slot gets fields for free.
  F1 adds zero teardown work here; this plan adds zero field work there.
- Audit before F1 lands: the shared field-store helpers extracted for brace-init parity
  (`TransferPointerOwnershipOnStore`, `RejectOwningValueCopyIntoField`, alias-reject,
  string deep-copy, ...) must classify "is a field store" by flag, not GEP shape, or
  interface fields leak on the brace-init path only.

## Open questions

- **Positional asymmetry (decide deliberately).** A bare local `Circle* c = new Circle();`
  is owning today; bare `list<Circle*>` becomes borrowed. Same spelling, opposite defaults
  by position. Two coherent endpoints: (a) accept it - "containers erase provenance, so they
  force you to say it; scopes can see it, so they don't" (where this plan lands); (b) the
  full journey - `unique` becomes the owning marker everywhere (locals, fields, containers)
  and bare `T*` uniformly means borrowed. Much bigger break; if (b) is the destination, this
  migration is the dress rehearsal. Take (a) now, record (b) as the standing question.
- **Move out of an interface field** (`string s = move iface.title;`): mutation-at-a-distance
  through a view. Language philosophy is consistent with allowing it (move nulls source,
  readable-as-null by design; mutation through borrows is legal everywhere). Forbidding
  first is the reversible choice. Needs a call when F1 lands.
- Does the same hole exist in `dictionary<K,V>` / `hashset<T>` with interface elements? Not
  checked - the mechanism (`is_pointer` false, `dataStructures` miss) is shared, so assume
  yes. The `unique` type-argument design covers them for free once `is_unique` exists;
  migrate them in the same pass (grep found 5 pointer-argument dictionary/hashset sites).
- Alias-reject cannot see through fat pointers (two views of the same object are statically
  indistinguishable). Accepted gap - same as raw pointers today; a `--sanitize` runtime
  data-pointer compare before owned-field stores is a cheap follow-up.
- Return-position and other conversion sites for unique interface values (ternary arms,
  brace-init of interface fields, `return move`, default args) - enumerate as a checklist,
  do not fix as-found (the brace-init parity lesson).

## Stages (IMPLEMENTATION STARTED 2026-07-18, on master; breaking change, other work held)

1. DONE 2026-07-18 (verified: test.sh Release 405/0/8). `unique` parses in type-argument
   position; canonical `"unique X*"` substitution text; distinct instantiation; D1
   diagnostic. Mangling: `list<unique Circle*>` -> `list__unique_Circleptr`. Grammar:
   `typeParameterEntry` takes an optional leading Identifier, validated as `unique` in the
   listener (keeps the soft keyword; no lexer token). Chokepoint `PeelTypeArgSuffix`
   strips the prefix for LLVM-type resolution; manual consumers touched:
   `ResolveSigComponentCodegen`, `GenerateDefaultValue`, qualified-name helper (~:11237).
   list.cb untouched - `is_pointer` stays true for `unique Circle*`, so unique lists
   behave as owning at this stage (asserted in Test/test_generics.cb).
2. DONE except `compile_error`: `is_unique(T)` landed with stage 1. The `compile_error`
   builtin (instantiation-error mechanism for list.cb's copy() rejection) moved to
   stage 5 where it is first needed.
3. DONE 2026-07-18 (verified: test.sh Release 407/0/8). Semantics for the DIRECT
   `unique X*` spelling on locals and params:
   - Synthesized move params (D4): both `ParseParameterTypeList` copies set IsMove when
     the param type is unique (single-indirection pointer or interface). Reuses the whole
     existing move-param machinery; unique-to-plain-param is automatically a borrow.
   - D5 decl-init rule: unique local must initialize from `new` / `move` / move-returning
     call / `nullptr`; borrowed source = error. Borrow-out already non-owning (verified
     no double-free). `delete` on a unique local rejected (assign nullptr to free);
     message wording says "unique field" - polish later.
   - Gap hardening, corrected findings: tuple and using-alias were NOT silently
     colliding (both mangle distinctly). using-alias of `list<unique X*>` works and was
     left working. Tuple element position now REJECTS unique (tuple has no dtor - an
     owning element would leak). The generic-function-call getText paths (both copies)
     were genuinely broken and now reject unique with a clear message.
   - Error strings (stable, used in Test/errors/): "unique requires a pointer or
     interface type; '<T>' is neither"; "cannot initialize unique '<name>' from a
     borrowed value - the source still owns it; use 'new', a 'move' expression, or a
     move-returning call, or drop 'unique'"; "unique is not supported as an explicit
     generic function type argument"; "unique is not supported as a tuple element type".
   - Tests: test_move.cb testUniqueLocalAndParam (4 dtor-count assertions); new
     err_unique_non_pointer_local / err_unique_borrow_into_unique /
     err_unique_rejected_position. DELETED err_unique_not_a_field.cb and
     err_unique_param.cb - both asserted the pre-D1 "only allowed on a struct field"
     restriction this design overturns.
   - SCOPING DEVIATION (deliberate, revisit at stage 5): post-substitution `unique` (a
     generic T bound to "unique X*") does NOT get D4/D5 semantics yet - synthesis there
     would break list.cb internals (`T tmp = _data[i]` sort helpers, the plan's known
     wrinkle) and turn borrow-params like `hashset.contains(T)` into move params.
     Container-level ownership semantics arrive with the stage-5 flip, where list.cb is
     rewritten against `is_unique` and its internals are adjusted in the same change.
   - No new serialized fields (reuses IsUnique/IsMove) - no --init change needed.
4. DONE 2026-07-18: F3 was ALREADY ON MASTER - `LowerInterfaceNullCompare`
   (MainListener.h:9258-9272, hooked at :9298) landed in commit db1624e with the
   interface-fields work, predating this plan; the earlier "spike-only" claim was wrong.
   Coverage added to Test/test_interface.cb (default-init / stack-boxed / heap-boxed,
   both operators, both operand orders; 35/35). Suite 407/0/8.
   REMAINING GAP found by boundary check, moved into stage 5: stack boxing into a unique
   interface (`Circle t; unique IShape s = t;`) is NOT rejected - the D5 decl-init rule
   does not fire for interface-typed unique locals (the two-liner above passes --check).
   Heap boxing `unique IShape s = new Circle();` works.
5. DONE 2026-07-18 (verified: test.sh Release 411/0/8; example_mac.sh 35/0; warm --init
   cache re-verified; the plan's Evidence repro rewritten as `list<unique IShape>` runs
   HeapAudit-clean, leaks=0). THE FLIP landed in one change:
   - `list.cb` ownership branches (dtor/set/removeAt/clear/copy) key off `is_unique(T)`.
     Bare `list<X*>` / `list<IFace>` is BORROWED (never frees); owning is
     `list<unique X*>` / `list<unique IShape>`.
   - `compile_error` builtin, DEFERRED-POISON mechanism: generic methods instantiate
     eagerly, so fire-on-instantiation would reject mere declaration. Instead it records
     the enclosing function in `poisonedFunctions`; `CheckPoisonedFunctionCalls()` errors
     only if a real CallBase user exists (vtable-slot references do not count). copy() on
     a unique list = "cannot copy a list of unique elements; use move or copy elements
     explicitly". KNOWN GAP: a poisoned method reached ONLY via virtual interface
     dispatch is not caught (no such call in-repo).
   - Stage-4 boxing gap fixed: stack boxing into `unique IShape` local rejected (D5
     message); heap `new` legal. err_unique_iface_stack_box.cb.
   - Interface container leg: `list<unique IShape>` frees via the vtable dtor slot -
     the branch that had never executed anywhere, now dtor-count + HeapAudit asserted in
     Test/test_collection_leaks.cb. Bare `list<IShape>` verified non-destroying.
   - Migration: per-site classification table in the stage-5 agent report (~30
     declarations across core/Test/example; owning -> unique, genuinely-borrowed stayed
     bare, e.g. ui_native.cb IElement lists owned by the reconciler tree, and one
     intentional borrowed-coexist list in test_generics). Windows-bound files
     (ui_native/win32.cb, example fsh.cb, todo_api.cb) migrated syntactically, not
     exercisable on macOS.
   - Sub-question resolved as planned: no transitional diagnostic; classification table
     + dtor-count oracles were the net.
   OPEN WART (needs a ruling): `add` kept its explicit `move T` param (required for
   value-type transfer), so `borrowedList.add(x)` still NULLS a named source. For a
   borrowed list fed from a named OWNING local this is a leak trap: ownership moves out
   of the local but the borrowed list will never free it. Borrowed adds are safe from
   rvalue borrows (`owner[i]`). Candidate fixes: a non-move add overload selected when
   `!is_unique(T) && is_pointer(T)`, or a diagnostic on moving an owning named local
   into a borrowed list. Decide before stage 6 spreads the pattern to dictionary/hashset.
6. `dictionary` / `hashset` same treatment.

## Related

- `internal/plan/field-ownership-unique.md` - the `unique` model this extends
- `internal/plan/interface-fields-feasibility.md` - F1; see "Interaction with interface
  fields" above
- `Test/test_interface.cb:95-106` - both boxing forms
- `Test/test_core.cb:639-661` - the owning-list API contract, future regression anchor
- `Test/test_collection_leaks.cb` - where the leak regression test goes
