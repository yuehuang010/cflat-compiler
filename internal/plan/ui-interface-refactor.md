# UI framework: pointers -> interfaces

Status: PHASE 1 + PHASE 2 + PHASE 3 COMPLETE (2026-07-13). F1-F4 are productionized and
shipped in the compiler, the framework-wide rename has landed, and the 22 widget
interfaces are declared and consumed by `core/ui_native.cb`. Phase 4 (hosts) is ready to
start - read the "Phase 4 inputs" section below first, it carries two hard constraints.
Created: 2026-07-13

## Goal

Refactor the cflat UI framework so that user-facing and internal code passes
elements as INTERFACE VALUES (fat pointers, type-checkable with `is` / `as`)
instead of raw class pointers (`Button*`, `View*`, `UiContext*`).

Two rules drive it:

1. Every interface is named with a leading `I` (`IElement`, `ICanvas`, ...).
   `core/interfaces.cb` already follows this (`IString`, `IList`, `IAllocator`);
   the UI framework is the only part of core that does not.
2. Every element class gets a matching interface (`Button` -> `IButton`), so a
   caller never needs a pointer to the concrete class.

## What is already done (do not redo)

`Element` IS ALREADY AN INTERFACE, and children are already stored as
`list<Element>` - i.e. interface values, not pointers (`core/ui_native.cb:753`,
`:799`). The tree spine is interface-based today. `Canvas`, `Component`, and
`NativeHost` are also already interfaces. Two examples (`winui_gallery.cb`,
`winui_app_demo.cb`) already contain ZERO pointer tokens.

So this refactor is NOT about the data structure. It is about the leaves of
usage: factories, downcasts, and field reach-through.

## The real cost: field reach-through

Measured over `core/ui_native.cb`, the three hosts, `ui_test.cb`, and
`example/ui/**`:

| Thing | Count |
|---|---|
| `X* v = e as X;` downcasts | 202 |
| ...of which reach a FIELD through the cast | 255 field touches |
| ...of which only call a METHOD | 64 calls |
| post-construction field writes through a factory pointer (examples) | 53 |
| `is` usages anywhere in the UI code | 0 |

The 4:1 field-to-method ratio is the whole problem. cflat interfaces are
METHODS-ONLY today (`CFlat.g4:843`), and JSX attribute lowering
(`MainListener.h:15955` `EmitOneFieldInit`) is definitionally a direct field
store. Three structural clusters generate nearly all 202 casts:

- `propsEqual(Element other)` - 23 sites, one per element class, all field compares.
- host `applyProps` / `createControl` / `nodeTooltip` - ~15-20 per host x 3 hosts, all field reads.
- host event routers - the cheap ones; these only call methods.

## Language gaps that block the design (compiler work, Phase 1)

| # | Gap | Today |
|---|---|---|
| F1 | Fields on interfaces | NOT SUPPORTED. Methods only. A field name on an interface receiver silently degrades to a method lookup (`MainListener.h:12167`). |
| F2 | `x is IFace` | NOT SUPPORTED. `GenerateIsCheck` accepts a concrete class only (`MainListener.h:7976`); an interface target is a compile error. |
| F3 | `ifaceValue != nullptr` | UNPROVEN. A failed `as <Interface>` yields a zeroed fat ptr (`MainListener.h:8005-8076`) but nothing in the codebase null-tests an interface value. This replaces the 202 `if (v != nullptr)` guards. |
| F4 | Implicit derived-iface -> parent-iface upcast (`IButton` -> `IElement`) | UNPROVEN. Only the explicit `as` form is tested (`Test/test_interface.cb:724`). |

F1 proposed design (UNDER VALIDATION): an interface field becomes a vtable slot
holding that implementor's BYTE OFFSET of the field. `b.title` lowers to
`dataPtr + offset[k]` cast to the field type - a true lvalue, no indirect call,
so reads, writes, and nested writes (`card.style.gap = 1`) all keep working and
JSX needs no change at all.

Consequence to watch: the vtable layout becomes
`[typedesc, methods..., fieldOffsets..., fullDtor]`, which MOVES the dtor slot
(currently `1 + methodCount`, `LLVMBackend.h:8606`). Delete-through-interface
regresses if that index is not updated.

Consequence for F4: parent interfaces are currently FLATTENED into the child's
vtable prefix (`LLVMBackend.h:6702`), so a parent vtable is a prefix of the
child's and an upcast could in principle be a reinterpret. Adding field slots
BREAKS that prefix property, so the upcast must instead rebuild the fat pointer
through the typedesc if-chain, like `as <Interface>` already does. Cost is
O(#implementors of the target); for a per-widget interface that is 1.

### Spike verdict (2026-07-13) - all four FEASIBLE

A working compiler prototype (~326 lines across `CFlat.g4`, `LLVMBackend.h/.cpp`,
`MainListener.h`) proved all four end-to-end, with `test.bat`, `test_lsp.bat`
(204/204), and `example.bat` (89/0/24) all green and no assertion weakened.

- F1 FEASIBLE. The byte-offset-slot design works as proposed, INCLUDING the
  nested-struct write (`b.style.gap = 1`), proven against two implementors with
  deliberately different field offsets. Settled vtable:
  `[typedesc, methods..., fieldOffsets..., fullDtor]`, with the dtor index now
  computed in ONE place (`InterfaceDtorSlotIndex`). Delete-through-interface
  still destructs exactly once.
- F2 FEASIBLE (trivial OR-chain over implementors).
- F3 FEASIBLE. It did NOT work before - the fat struct reached `CreateOperation`
  as an aggregate. Now lowers to a data-pointer compare.
- F4 FEASIBLE-WITH-CAVEATS. Assignment, call arg, and `list<IParent>.add` work;
  RETURN POSITION IS NOT WIRED (must be finished in Phase 1). The vtable prefix
  property was rejected as an upcast mechanism - it is only accidentally correct
  today and already breaks for a second parent - so the upcast rebuilds the fat
  pointer through the typedesc if-chain, as predicted.

Landmine found and fixed in the spike, worth remembering: the field-store
ownership rules classify a store as "struct field" STRUCTURALLY (a GEP with 2
indices). An interface field's address is a BYTE gep, so destruct-before-overwrite
was silently skipped and an owned `string` assigned through an interface field
LEAKED. Fixed with an explicit `IsInterfaceField` flag. Any other code that
sniffs GEP shape to infer intent has the same blind spot.

## Scope (agreed)

IN:
- The element tree + all 23 widget classes -> `IElement` + one interface each.
- `UiContext*` -> `IUiContext` (125 occurrences; a struct today, only `.theme` is
  field-reached: 64 reads, 1 write).
- `UiTest*` -> `IUiTest` (24 occurrences, 100% method calls - free conversion).

OUT (for now):
- `list<Element>* childList()` - the last raw pointer in the `Element` contract.
  It is a pointer-to-LIST, not a pointer-to-element. Left as-is; revisit later as
  `childCount()` / `childAt(i)` / `setChildAt(i, e)`.
- The concrete classes stay public with public fields: JSX `<Button title="..."/>`
  requires the tag to name a concrete class with a public field and an `add()`
  method (`MainListener.h:15941`). Classes remain the CONSTRUCTION type; the
  interface is the USAGE type.

## Phases

Sequencing is COMPILER FIRST: land the language features against a green,
untouched UI framework, so a bisect can tell a language bug from a UI bug.

### Phase 1 - Language - DONE (2026-07-13)

All six items landed. Gates: `test.bat` all passed, `test_lsp.bat` green (5 smoke +
47 fixture + 204 bulk), `example.bat` 89 passed / 0 failed / 24 skipped. The UI framework
was not touched.

1. RETURN-POSITION upcast - DONE (`MainListener.h`, the return path). A returned
   fat pointer whose static interface differs from the declared return interface is
   rebuilt through the typedesc chain, like the assignment/arg paths. The two existing
   ownership guards are untouched: both fire on a returned concrete POINTER, which an
   already-boxed interface value is not.
2. BITCODE CACHE - DONE. Interface records now round-trip a `fields` array
   (`LLVMBackend.cpp`), and the metadata `version` went 1 -> 2 so any pre-existing
   cache is rejected instead of silently dropping fields. Verified end-to-end by
   temporarily adding a field-carrying interface to `core/interfaces.cb`, running
   `--init`, and reading the field through the interface on a cache HIT.
3. EAGER validation - DONE. `LLVMBackend::VerifyInterfaceFields` runs at the CLASS
   (via `VerifyInterfaceImplementation`) and at the `program` definition, and names the
   class, the interface, the field, and the expected type. `AppendInterfaceFieldOffsetSlots`
   no longer reports (it trusts the eager check), so there is exactly one message.
   Tests: `Test/errors/err_iface_field_missing.cb`, `err_iface_field_type_mismatch.cb`.
4. NULL interface field access - DECIDED: leave it as a runtime fault, document it.
   It is exactly what a METHOD call on a null interface value already does, cflat has no
   static null tracking to catch it at compile time, and F3's `!= nullptr` compare is the
   guard (now documented in `doc/LANGUAGE.md`). Adding a runtime null check on every field
   read would tax the hot path of the UI reconciler for a bug the type system does not
   otherwise catch.
5. LSP - DONE. Interface fields register as `IFace.field` symbols from their own
   declaration site; an INHERITED field is also registered under the derived interface
   (pointing at the parent's declaration), so dot-completion on a derived interface shows
   the whole flattened set. Four new fixtures under `cflat/test_lsp/fixtures/`
   (hover, completion own-field, completion inherited-field, go-to-definition).
6. Untested surface - BOTH WORK, no compiler change was needed:
   - Generic-interface fields (`interface IHolder<T> { T value; }`) work, including two
     implementors with different layouts. Covered by test 28 in `Test/test_interface.cb`.
   - `program X : IFace` implementors work: the config fields are the implementor's
     fields and the program vtable carries their offsets. Covered by
     `testProgramInterfaceField` in `Test/test_program.cb`.

MULTI-LEVEL CHAINS WORK (the Phase 3 gating question). A 3-level chain
(`IPress : ITipped : IWidget`) with a field introduced at EVERY level was verified end to
end against two implementors with deliberately reversed layouts: flattening is transitive
and identical for methods and fields (each level prepends its parent's already-flattened
list), so the interface's slot order and every implementor's vtable agree by construction;
the dtor slot is computed from the flattened counts at each level and still fires exactly
once; upcasts work child->parent AND child->grandparent in all four positions (assignment,
call arg, `list<IAncestor>.add`, return); and `is` / `as` against an ancestor two levels up
work. **Phase 3 can use the `IElement -> ITooltipped -> IDisableable -> IButton` design; it
does not need to repeat shared fields across the 23 widget interfaces.**

Known gaps left open deliberately (neither blocks the UI refactor):
- Returning an interface value that BORROWS a stack local (`Circle c; IShape s = c;
  return s;`) dangles and is not diagnosed. The direct form (`return c;`) is rejected.
  Documented in `doc/LANGUAGE.md`.
- An implicit interface->interface conversion that is NOT an upcast (unrelated or
  downward) silently yields a null fat pointer instead of erroring, because the static
  type of an `as`-cast result is not tracked precisely enough to tell the two apart
  (`IButton b = e as IButton;` reaches the assignment still carrying `IElement`). The
  runtime rebuild is what makes that pattern correct, so it must not error.
- GEP-shape sniffing audit (the spike's landmine): the ONLY places that infer "this is a
  struct-field store" from a 2-index GEP are `MainListener.h` `destIsStructField` /
  `srcIsStructField`, and both already OR in `IsInterfaceField`. The other GEP tests in
  the codebase (`isa<GetElementPtrInst>` in the deref/load paths, `GetTypeFromStorage`)
  are shape-agnostic and treat the interface field's re-GEP correctly. Verified
  leak-clean under HeapAudit: overwriting an owned `string` through an interface field
  frees the old buffer.
- `list<IFace>` bugs found while testing, both PRE-EXISTING and unrelated to fields.
  BOTH ARE NOW CLOSED (2026-07-13), so Phase 3 can lean on `list<IElement>`:
  - The heap corruption when a LOCAL `list<IFace>` had an element taken out and deleted
    was REAL and is FIXED. Root cause: passing an owning `T*` to a `move IFace` parameter
    boxed it into a fat pointer and handed ownership to the callee, but the caller kept
    its owning flag, so scope exit freed it again. The interface-borrow guard in
    `CreateOverloadedFunctionCall` now excludes POINTER arguments (a struct VALUE arg
    still borrows - its fat ptr points at the caller's alloca). Covered by
    `Test/test_interface.cb` tests 29-31.
  - The 64-byte buffer leak was DISPROVEN - a HeapAudit measurement artifact
    (`reportLeaks()` was called while the list was still live in scope). See
    `internal/issue/list-of-interface-leaks-buffer.md`; nothing to fix.

### Phase 2 - Rename (mechanical, no semantics) - DONE (2026-07-13)
`Element`->`IElement`, `Canvas`->`ICanvas`, `Component`->`IComponent`,
`NativeHost`->`INativeHost`, across `core/ui_native.cb`, `core/ui_native/*.cb`,
`core/ui_canvas/*.cb`, `core/ui_test.cb`, `core/cocoa.cb`, `example/ui/**`, `doc/UI.md`.
28 files, 624 insertions / 624 deletions, zero compiler files touched. The one
`"type":"Component"` STRING LITERAL in `toJson` is deliberately NOT renamed - it is a
serialized JSON contract. Gate: all three suites green.

### Phase 3 - Widget interfaces - DONE (2026-07-13)

`core/ui_native.cb` only (the sole file changed). 22 widget interfaces declared; all 32
concrete-class downcasts in the file are gone (grep for ` as <ElementClass>` returns only
a prose comment). Gates: `test.bat` all passed, `test_lsp.bat` 204/204, `example.bat`
89 passed / 0 failed / 24 skipped and leak-clean. Hosts, factories, contexts, and
examples were NOT touched and still compile against the concrete classes unchanged.

Shipped hierarchy (see `internal/plan/ui-widget-interface-inventory.md` for the evidence):

```
IElement
  +- ITooltipped  { string tooltip; }                 // 15 implementors
  |    +- IDisableable { bool disabled; }             // 10 implementors
  |    |    +- IButtonElement, ITextInput, ICheckbox, ISlider, ITextArea,
  |    |       IRadioButton, IComboBox, IListView, ITabControl, ITreeView
  |    +- IText, IProgressBar, IImageElement, IGroupBox, ICanvasView
  +- IView, IBox, IStatusBar, IScrollView, IRadioGroup, ITabPane, ISplitView
```

The `ITooltipped -> IDisableable` chain the inventory recommended is in, and the
multi-level upcast it depends on works. `ComponentElement` got no interface (zero
downcasts repo-wide), so 22 interfaces for 23 classes.

Deviations from the inventory, all deliberate:

1. **`IButton` and `IImage` are named `IButtonElement` / `IImageElement`.** HARD NAME
   COLLISION, not a style choice: `core/ui_native/winui.cb` consumes the WinUI winmd,
   which projects a WinRT `IButton` (used at `winui.cb:117`) and a WinRT `IImage`
   (`:587-589`, `iidof(IImage)` + `put_Source` through its vtable). A WinMD projection
   and a cflat interface cannot share a name - the WinMD type wins the lookup and the
   cflat interface value is then GEP'd as a COM struct, producing INVALID IR ("Invalid
   bitcast ... %__iface_fat_ptr", module verification failure), not a diagnostic. Fully
   qualified WinMD names (`Microsoft.UI.Xaml.Controls.IImage`) are NOT supported, so no
   spelling in winui.cb can dodge it. See "Phase 4 inputs" below.
2. **9 of the 14 closure fields are NOT interface fields.** They are reached only to be
   INVOKED, so they got fire* wrapper methods on the class (mirroring the existing
   `fireClick`/`fireToggle`/`fireSet`): `TextInput.fireChangeText`, `TextArea.fireChange`,
   `RadioGroup.fireChange`, `ComboBox.fireChange`, `ListView.fireSelect`/`fireActivate`,
   `TabControl.fireSelectTab`, `TreeView.fireSelect`/`fireExpand`,
   `SplitView.fireRatioChange`. The 5 closures the hosts must READ AS VALUES (to clone
   into a box across the u64 `INativeHost` seam) ARE interface fields:
   `IListView.rowText`, `ITreeView.childCount`/`childId`/`label`, `ICanvasView.onPaint`.
3. `RadioButton.bounds` is not an interface field - `RadioGroup.dispatch` now uses
   `IElement.nodeBounds()`, as the inventory recommended.
4. `TabControl.addPane(TabPane* p)` is NOT on `ITabControl` (it takes a concrete pointer
   and no host calls it). `ITabControl.add(IElement)` covers the interface contract.

All 23 `propsEqual` bodies keep their exact semantics - only the downcast type changed
(`X* o = other as X` -> `IX o = other as IX`). The 5 deliberately special-cased ones are
unchanged: `TextArea` still ignores `value`, `ListView` still ignores its 3 closures and
its column CONTENTS (count only), `Image` still ignores `source`/`altText`/`tooltip`,
`CanvasView` still compares nothing, `ComponentElement` still returns `true` (and never
had a cast).

### Phase 4 inputs (read before starting the hosts)

1. **Lambda-typed interface fields WORK.** Proven end to end against two implementors
   with deliberately different field offsets: reading the field BY VALUE into a by-value
   `Lambda` parameter CLONES (the field is still invocable afterwards - this was the top
   risk, E1 in the inventory), invoking through the field works, and overwriting the
   field works. Leak-neutral: a HeapAudit run through the interface field and through the
   equivalent concrete-class pointer report byte-identical results. So
   `IListView.rowText` / `ITreeView.*` / `ICanvasView.onPaint` can be passed straight to
   `_boxListRow` / `_boxTree` / `_boxCanvas` in Phase 4.
   Owning-container interface fields (`list<string>`, `list<int>`, `list<IElement>`) are
   also true lvalues - `o.items.count()` and `o.items.add(...)` hit the object, not a
   copy (E2 clear).
2. **A cflat interface name is unusable if ANY file in the import closure names a WinMD
   type of the same name.** Today that is exactly `IButton` and `IImage` (both from
   `winui.cb`). It is NOT a general WinUI-namespace clash: `IListView`, `ISlider`,
   `IComboBox`, ... all exist in the WinUI winmd too but resolve to the cflat interface,
   because winui.cb never names them as WinRT types. Phase 4 must not introduce a WinRT
   reference whose short name matches a widget interface. Two follow-ups worth doing,
   neither blocking:
   - The compiler should ERROR on this collision instead of emitting invalid IR.
   - `winui.cb:117` uses WinRT `IButton` only as an arbitrary IUnknown stand-in for the
     slot-0 QI (its own comment says so). Switching it to `IButtonBase` (already imported
     and used at `:437`) would free the `IButton` name; `IImage` at `:587-589` is a real
     use and cannot be freed without qualified-name support.
3. Pre-existing bug found while validating (does not block Phase 4, but do not build on
   it): calling a `Lambda<string(...)>` CLASS FIELD and binding the owned result to a
   local leaks the string. See `internal/issue/lambda-field-call-owned-string-result-leaks.md`.

### Phase 4 - Hosts
`win32.cb` (52 casts), `cocoa.cb` (52), `winui.cb` (50): `applyProps`,
`createControl`, `nodeTooltip`, `isEnabled`, and the event routers move to
`IX` values. NOTE: cocoa cannot be runtime-verified here (no Mac box) - it is
compile-checked only, and must be validated on Apple Silicon before release.

### Phase 5 - Factories
The 24 factory signatures (`view`, `button`, `scrollView`, ... `ui_native.cb:2261+`)
return interface values (`move IButton button(...)`) instead of `Button*`.
JSX is unchanged: `<Button/>` still news a concrete `Button` and auto-boxes at
the `IElement` slot.

PREREQUISITE (landed 2026-07-13, do not regress): the `move IFace` double-free fix.
Passing an owning pointer to a `move` interface parameter used to leave the caller
owning it too - a double free. This is exactly the shape Phase 5 introduces, since a
factory hands a freshly-`new`ed widget into the tree by move. The shipped framework
never hit it only because `View.add(IElement child)` is NOT a `move` param today (the
caller keeps ownership and the tree borrows); the moment factories return
`move IButton` and children are added by move, every add goes through this path.
Both call paths are covered now:
- direct calls: `CreateOverloadedFunctionCall` (interface-borrow guard excludes pointers)
- virtual dispatch: `CallInterfaceMethod` had NO move-param source-nulling AT ALL; both
  paths now share `LLVMBackend::ApplyMoveParamTransfer` so they cannot drift again.
Regression cover: `Test/test_interface.cb` tests 29-32.

### Phase 6 - Contexts
`UiContext` struct -> class + `IUiContext` (the 64 `.theme` reads need a field or
an accessor - F1 decides which). `UiTest*` -> `IUiTest`; the case-body signature
`function<void(UiTest*)>` (`ui_test.cb:270`) becomes `function<void(IUiTest)>`.

### Phase 7 - Examples + docs
19 example files (~290 pointer lines; `gallery_app.cb` 76, `tui_demo.cb` 57,
`fedit.cb` 29 are the big three) and `doc/UI.md` (~58 lines).

## Gates (every phase)

- `test.bat` (Release) green
- `test_lsp.bat` green
- `example.bat` green AND leak-clean (the UI examples self-test under HeapAudit)
- No test assertion weakened to make a phase pass.

## Risks

1. F1 is the whole plan. If interface fields do not work, the ergonomics
   collapse into accessor-method soup and the value of the refactor drops sharply.
2. Ownership. `list<T>` does NOT destruct interface elements
   (`core/list.cb:244`); the UI tree is freed by hand via `destroyTree()` +
   `delete c` through the fat pointer (`ui_native.cb:1242`). Every phase must stay
   HeapAudit-clean; interface locals are never auto-destructed by design
   (`LLVMBackend.h:1752`).
3. Boxing semantics differ by source kind: boxing an owning HEAP pointer into an
   interface MOVES ownership (source is nulled); boxing a STACK value BORROWS
   (`Test/test_interface.cb:909`). Factories return heap pointers, so Phase 5
   changes ownership flow - watch for leaks and moved-into-interface delete errors.
4. The dtor vtable slot moves under F1. Delete-through-interface is covered by
   `Test/test_interface.cb:570` (dtor-count assertions) - keep it passing.
5. Cocoa is compile-only verified on this box.

## Estimated size

~1,190 lines across the framework, of which ~255 are the irreducible field
reach-throughs, plus the compiler work in Phase 1.
