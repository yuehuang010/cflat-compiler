# UI framework: pointers -> interfaces

Status: **COMPLETE (2026-07-13). ALL SEVEN PHASES DONE.** F1-F4 are productionized and
shipped in the compiler, the framework-wide rename has landed, the 22 widget interfaces
are declared and consumed by `core/ui_native.cb`, all three native hosts reach every
widget through its interface, every factory returns its widget interface by `move`, both
contexts (`IUiContext`, `IUiTest`) are interfaces, and the examples + `doc/UI.md` are on
the new API. ZERO concrete-class downcasts remain anywhere in the framework, the hosts, or
the examples. The only runtime gap left is Mac validation (see the checklist at the bottom).
Created: 2026-07-13

## What changed for users

Concrete classes are the CONSTRUCTION type (what `new`, a factory, and a `<Button/>` sugar
tag build). Interfaces are the USAGE type (what you declare, pass, store, and return). You
should not need a `Button*` in app code anymore.

| Before | After |
|---|---|
| `Button* b = button("Save", onPress);` | `IButton b = button("Save", onPress);` (factory returns `move IButton`) |
| `Button* b = <Button title="Save"/>;` | `IButton b = <Button title="Save"/>;` (the tag still names the CLASS; it auto-boxes) |
| `IElement render(UiContext* ctx)` | `IElement render(IUiContext ctx)` |
| `s.test("case", (UiTest* t) => {...});` | `s.test("case", (IUiTest t) => {...});` |
| `View* v = tree as View; if (v != nullptr)` | `IView v = tree as IView; if (v != nullptr)` |
| `tabs.addPane(p);` | `tabs.add(p);` (`addPane` was dropped - `add(IElement)` covers it) |
| n/a | `if (node is ITooltipped) { ... }` - `is` now accepts an interface |
| a field needed a concrete downcast | interface FIELDS are true lvalues: `b.title = "x"`, `card.style.gap = 1` |

Ownership is unchanged in FLOW and now DECLARED: a factory returns `move IX` (the caller
owns the new node), `parent.add(child)` PUBLISHES rather than transfers (so
configure-after-insert still works), and the tree is freed by `destroyTree()` + `delete c`
/ `deleteTree(root)`. An interface local is never auto-destructed, so there is never a
second owner.

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
  |    |    +- IButton, ITextInput, ICheckbox, ISlider, ITextArea,
  |    |       IRadioButton, IComboBox, IListView, ITabControl, ITreeView
  |    +- IText, IProgressBar, IImage, IGroupBox, ICanvasView
  +- IView, IBox, IStatusBar, IScrollView, IRadioGroup, ITabPane, ISplitView
```

The `ITooltipped -> IDisableable` chain the inventory recommended is in, and the
multi-level upcast it depends on works. `ComponentElement` got no interface (zero
downcasts repo-wide), so 22 interfaces for 23 classes.

Deviations from the inventory, all deliberate:

1. **RESOLVED - `IButton` and `IImage` have their natural names.** They shipped as
   `IButtonElement` / `IImageElement` because a WinMD projection and a cflat interface
   could not share a name (the WinMD type won the lookup and the cflat interface value -
   a fat pointer - was GEP'd as a COM struct: INVALID IR, no diagnostic). WinMD types are
   now registered ONLY under their fully-qualified name
   (`Microsoft.UI.Xaml.Controls.IButton`), so the collision class no longer exists and
   both interfaces were renamed back. `winui.cb` names WinMD types through prefixed
   `using` aliases (`XamlButton`, `XamlImage`, ...); a core library must not claim bare
   identifiers. See `doc/WINMD.md` "WinMD Types Are Always Fully Qualified".
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
2. **RESOLVED - a cflat interface name can no longer be taken by a WinMD type.** WinMD
   types are registered only under their fully-qualified WinRT name, so nothing in the
   import closure can claim a bare identifier. Phase 4 is free to name any widget
   interface. The one remaining way to collide is a `using` alias that deliberately
   claims a name a cflat interface also uses - that is now a hard compiler error
   (`Test/errors/err_winmd_alias_collides_with_interface.cb`), never a silent shadow.
3. Pre-existing bug found while validating (does not block Phase 4, but do not build on
   it): calling a `Lambda<string(...)>` CLASS FIELD and binding the owned result to a
   local leaks the string. See `internal/issue/lambda-field-call-owned-string-result-leaks.md`.

### Phase 4 - Hosts - DONE (2026-07-13)

The three host files were the only ones changed (`cflat/core/ui_native/win32.cb`,
`winui.cb`, `cocoa.cb`). Every concrete-class downcast is gone: a grep for
` as <ElementClass>` across the three returns ZERO. Gates: `test.bat` all passed,
`test_lsp.bat` 204/204, `example.bat` 89 passed / 0 failed / 24 skipped and leak-clean
(gallery.cb + winui_gallery.cb both self-test green). `ui_native.cb`, the factories, the
contexts, and `example/ui/**` were NOT touched and still compile against the concrete
classes unchanged.

| Host | Concrete downcasts before | Interface downcasts after |
|---|---|---|
| `win32.cb` | 52 | 38 |
| `winui.cb` | 50 | 42 |
| `cocoa.cb` | 52 | 38 |
| **Total** | **154** | **118** |

The 36-cast net reduction is entirely the two hoisted interfaces collapsing the two
type-switch cascades the inventory predicted:

- `tooltipOf()` - a 15-way `as`-chain in BOTH win32 and cocoa (30 casts) collapsed to
  ONE `ITooltipped t = node as ITooltipped;` each. 30 -> 2.
- `nativeIsEnabled()` in winui - a 10-way chain collapsed to one
  `IDisableable d = e as IDisableable;`. 10 -> 1. The `d == nullptr` case IS the old
  fallthrough (`return true`, "controls without a disabled prop are always enabled"), so
  the semantics are identical by construction.

Everything else is a 1:1 type-level rename (`X* v = node as X` -> `IX v = node as IX`),
since `applyProps`/`createControl` dispatch on `kind()` and need the per-widget contract.
`nativeIsChecked` (winui) keeps its 2-way branch: `checked` lives on `ICheckbox` and
`IRadioButton` separately (no shared interface carries it), which is what the inventory's
summary A predicted.

No interface field was missing. Every field the three hosts reach - including the
single-host ones (`ScrollView.contentH` cocoa-only, `Image.source` + `RadioButton.groupFirst`
win32-only) - was already on the Phase 3 union, so `ui_native.cb` needed NO change.
The three host-internal helpers that took a concrete pointer now take the interface:
`_syncStatusBar(u64, IStatusBar)`, `_syncCombo(u64, IComboBox)`, `_syncTabs(u64, ITabControl)`
(+ cocoa's `_syncButtonAccent(u64, IButton)`).

The event routers use the fire* wrappers throughout (`fireChangeText`, `fireChange`,
`fireSelect`, `fireActivate`, `fireSelectTab`, `fireExpand`, `fireRatioChange`), so no
closure field is reached to be INVOKED. The 5 closures that are READ AS VALUES to be boxed
across the `INativeHost` u64 seam - `IListView.rowText`, `ITreeView.childCount`/`childId`/
`label`, `ICanvasView.onPaint` - are read straight off the interface and passed to
`_boxListRow` / `_boxTree` / `_boxCanvas` exactly as before. The clone-on-by-value-read
proven in Phase 4 inputs holds: the gallery re-renders and stays leak-clean.

COCOA IS COMPILE-CHECKED ONLY (no Mac box). It passes
`cflat.exe cflat/core/ui_native/cocoa.cb --check --platform macos` and the `test_lsp.bat`
bulk sweep, and its changes are a line-for-line mirror of the win32 patterns (no logic was
touched). It MUST be validated on Apple Silicon (`./test.sh Release` + a real gallery run)
before release.

### Phase 5 - Factories - DONE (2026-07-13)

All 25 factories (the 24 widget factories + `mount`) return their widget interface by
`move`. JSX is unchanged: `<Button/>` still news a concrete `Button` and auto-boxes at
the `IElement` slot.

| Factory | Now returns |
|---|---|
| `view` / `row` / `column` / `toolbar` | `move IView` |
| `text` | `move IText` |
| `button` | `move IButton` |
| `box` | `move IBox` |
| `textInput` | `move ITextInput` |
| `checkbox` | `move ICheckbox` |
| `progressBar` | `move IProgressBar` |
| `slider` | `move ISlider` |
| `scrollView` | `move IScrollView` |
| `textArea` | `move ITextArea` |
| `statusBar` | `move IStatusBar` |
| `radioGroup` | `move IRadioGroup` |
| `comboBox` | `move IComboBox` |
| `listView` | `move IListView` |
| `tabPane` | `move ITabPane` |
| `tabControl` | `move ITabControl` |
| `treeView` | `move ITreeView` |
| `splitView` | `move ISplitView` |
| `image` | `move IImage` |
| `groupBox` | `move IGroupBox` |
| `canvasView` | `move ICanvasView` |
| `mount` | `move IElement` |

`mount` returns `move IElement` because `ComponentElement` deliberately has no interface
of its own (Phase 3: zero downcasts reach a field of it) and `IElement` is the entire
contract a caller needs - it is only ever handed straight to a parent's `add()`.

**OWNERSHIP DECISION: `add(IElement child)` STAYS A BORROW.** The contract is now written
above `interface IElement` in `ui_native.cb`:

- A factory returns `move IX`, so the CALLER owns the freshly-`new`ed node. (This is
  forced, not chosen: returning an owning heap pointer boxed into a non-`move` interface
  return is a compile error.)
- `add()` does not TRANSFER, it PUBLISHES: after `parent.add(child)` the parent's
  `children` list names the node, and from that point the TREE is its single owner.
- That is sound because an interface value carries no ownership bit and an interface local
  is NEVER auto-destructed (`LLVMBackend.h` `EmitDestructorsForScope`: interfaces are not
  in `dataStructures`, so no dtor is emitted; an owning `IsOwning` flag on a fat-ptr local
  is inert). So a caller that boxed a node and handed it to a parent has nothing left to
  free - there is no window in which two owners exist and no scope exit that could
  double-free.
- Making `add` `move IElement` would buy nothing and cost something. Nothing: for an
  INTERFACE-typed argument `ApplyMoveParamTransfer` classifies the fat ptr as a borrow
  anyway (`isInterfaceBorrow`), so no source-nulling would be emitted. Cost: `move` would
  mark the local moved, forbidding the configure-after-insert idiom
  (`root.add(btn); btn.title = ...;`) that the framework and examples use everywhere.
- A node never given to a parent is the caller's to `delete` (through the fat pointer's
  dtor slot). The tree is freed exactly once, by `destroyTree()` + `delete c` per child
  (`list<IElement>` does not destruct its elements). Unchanged from before Phase 5.

Net effect on ownership FLOW: none. Before Phase 5 a factory returned a raw `T*` that the
compiler did not track as owning (a non-`move` pointer return), so nothing freed the local
and the tree owned the node. After Phase 5 the caller's local is an interface value, which
is likewise never auto-destructed, and the tree owns the node. The difference is that the
transfer is now DECLARED (`move`) instead of implied by a convention the type system could
not see.

Three compiler changes were required (all in `MainListener.h`, all landed). The first is the
one this phase was warned about - and it did NOT surface as a compile error or a leak, it
surfaced as a hard 0xC0000005 in EVERY UI example at teardown:

0. **THE REAL BUG: the derived->parent interface upcast was silently skipped on the
   VIRTUAL-DISPATCH argument path.** `v.add(text("hi"))` - an `IText` value passed to
   `IView.add(IElement)` - stored the `IText` vtable in an `IElement` slot. Methods still
   worked (a parent's methods are a PREFIX of the child's flattened vtable, so their indices
   coincide), which is why `toJson()` printed a perfect tree. But the DTOR SLOT does not
   coincide: `InterfaceDtorSlotIndex(IElement)` = 14, while slot 14 of `Text_IText_vtable` is
   `inttoptr (i64 24)` - the byte offset of the `tooltip` FIELD. So `destroyTree()`'s
   `delete c` called address 0x18. That is exactly the observed fault address.
   Root cause: the arg-marshalling in the interface-method-call path took the argument's
   `TypeName` from its LLVM struct type, which for ANY interface value is the shared
   `__iface_fat_ptr`. `ReboxInterfaceIfNeeded("__iface_fat_ptr", "IElement")` sees a
   non-interface source and returns the value untouched - no upcast, no diagnostic. The
   free-function call path already propagated the interface NAME correctly; the virtual path
   did not. It now does, so the two agree. Before Phase 5 nothing ever passed a DERIVED
   interface value to a PARENT-interface param through virtual dispatch (factories handed out
   concrete pointers, which take the concrete->interface branch), so the gap was unreachable.
   Verified fixed: `Text_IElement_vtable` is now emitted and the upcast if-chain appears.

The other two:

1. **BUG FIX - stale ownership flag on an interface declaration.** `ParseDeclaration` consumed
   `lastCallReturnsOwned` only for a `string` or a POINTER local. An INTERFACE local
   (`IStatusBar sb = statusBar("hi");` - the shape Phase 5 introduces) left the flag SET,
   so it leaked into the next declaration or return and misclassified it as owned. Symptom:
   a spurious *"returning a heap object boxed into interface 'IElement' from a non-'move'
   function ... it will leak"* on a perfectly good `return root;` in `fedit.cb`, and (when
   the interface local came FIRST) a following `T*` local wrongly marked owning - which
   would have been a double free at runtime. The declaration path now consumes the flag for
   an interface local and marks it `IsOwning` (inert at scope exit, but it is what makes
   `return t;` from a `move IText` factory type-check as owned).
2. **NEW DIAGNOSTIC.** Returning an interface VALUE from a function declared to return a
   concrete pointer (`View* _card(...)` whose body now returns an `IView`) reached LLVM as a
   fat ptr against a pointer return type and failed module verification with no source
   location. It is now a clean error. Regression case added to the existing
   `Test/errors/err_return_interface_value.cb` (no new test file).

Also dropped: `TabControl.addPane(TabPane* p)` - the last concrete-element-pointer method in
the framework, and a pure alias of `add(IElement)` (Phase 3 deviation 4 predicted this).
`fedit.cb` / `fedit_jsx.cb` / `doc/UI.md` updated to `add()`.

One interface field was added: `ISplitView.onRatioChange` (`Lambda<void(int)>`). It is a
settable prop with no factory argument, so a caller holding an `ISplitView` had no way to
wire it (`fedit.cb` does). The other 8 non-interface closure fields stay off the interfaces -
every one of them IS a factory argument, so nothing needs to reach them.

Example ripple (PURELY MECHANICAL - `Button* b = button(...)` -> `IButton b = button(...)`;
Phase 7 still owns the JSX decls, the `as`-downcasts, and `doc/UI.md`): 14 files touched -
`01-elements/{app,counter,counter_jsx}.cb`, `02-terminal/{boxes,tui_demo}.cb`,
`03-canvas-win32/{win32_boxes,win32_settings,win32_shot}.cb`,
`04-native-controls/{win32,cocoa}_native_settings.cb`, `05-gallery/gallery_app.cb`,
`06-winui/winui_demo.cb`, `07-testing/todo_app.cb`, `08-fedit/{fedit,fedit_jsx}.cb`.
Three example function signatures went with the factories: `tui_demo.ktext` ->
`move IText`, `gallery_app._card` -> `move IView`, and `fedit`'s two `addPane` calls ->
`add`. `fedit_jsx.buildTabs` stays `TabControl*` (pure JSX, no factory).

Left for Phase 7 (unchanged by this phase): the JSX-result decls
(`Button* btn = <Button .../>;` - still correct, JSX constructs the CONCRETE class), the
`tree as View` downcasts in `counter.cb`, and the `doc/UI.md` factory signature table
(which still advertises `View* view(Style)`).

Gates: `test.bat` all passed, `test_lsp.bat` 204/204, `example.bat` 89 passed / 0 failed /
24 skipped and LEAK-CLEAN.

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

Inputs from Phase 4 (nothing here blocks Phase 5):

1. The hosts are now interface-clean, so a factory returning `move IButton` instead of
   `Button*` has NO host-side consumer to update - the hosts already only see `IElement`
   coming out of the tree and downcast to `IX` themselves. The blast radius of Phase 5 is
   `ui_native.cb`'s 24 factory signatures + `example/ui/**` (Phase 7), NOT the hosts.
2. The 4 host-internal helpers that used to take a concrete element pointer now take an
   interface (`_syncStatusBar(u64, IStatusBar)`, `_syncCombo(u64, IComboBox)`,
   `_syncTabs(u64, ITabControl)`, cocoa `_syncButtonAccent(u64, IButton)`). These are
   BORROW params (a plain `IFace`, not `move IFace`), so they are unaffected by the
   move-param transfer rule - do not convert them to `move`.
3. `ITabControl.addPane(TabPane* p)` was already excluded from the interface (Phase 3
   deviation 4). If Phase 5 changes `tabPane()`'s return type, `addPane` must go with it
   or be dropped in favour of `ITabControl.add(IElement)`, which already covers it.

### Phase 6 - Contexts - DONE (2026-07-13)

Both conversions landed. `UiContext*` -> `IUiContext` (all ~125 occurrences) and
`UiTest*` -> `IUiTest` (all ~24). ZERO `UiContext*` / `UiTest*` tokens remain in
`cflat/core/**` or `example/**`. Gates: `test.bat` all passed, `test_lsp.bat` 5 smoke +
47 fixture + 204 bulk = green, `example.bat` 89 passed / 0 failed / 24 skipped and
LEAK-CLEAN.

**`IUiContext`** - `UiContext` is now `class UiContext : IUiContext`. Contract:

- ONE field: `Theme theme;`. The F1 byte-offset slot makes it a true lvalue through the
  fat pointer, so all 64 `ctx.theme.<field>` NESTED READS (`ctx.theme.buttonBg`) and the
  whole-struct WRITES (`ctx.theme = darkTheme();` in 8 example `render()` bodies) work
  unchanged - no accessors were invented. `&ctx.theme` (the hosts' `themeIsDark(&ctx.theme)`)
  also works: address-of an interface field yields the field's address.
- 20 methods: `invalidate`, `requestRepaint`, `consumeDirty`, `consumeRepaint`, `focus`,
  `blur`, `hasFocus`, `setHover`, `hasHover`, `setPress`, `clearPress`, `hasPress`,
  `setNativeHandle`, `hasNativeHandle`, `nativeHandle`, `removeNativeHandle`, `bindPost`,
  `bindThreadId`, `post`, `assertUiThread`.

IDENTITY IS PRESERVED - boxing does NOT copy and introduces NO owner. The concrete
instance still lives exactly where it did (`Win.ctx` per host window in win32/cocoa, the
`_wCtx` / `_gCtx` global in winui / ui_canvas, a stack local in the headless examples), and
`activeCtx()` returns `&<that instance>` boxed into `IUiContext` - a fat pointer whose data
word IS the instance's address. Boxing a value or a `&`-address BORROWS (the source keeps
ownership and is not zeroed), an interface local is never auto-destructed, and nothing
`delete`s a context. Proven behaviourally: state written through one `IUiContext`
(focus / hover / press / `nativeByKey` / `theme`) is read back through a later, independent
`activeCtx()` by the self-tests (`UiTest.exists()`, the `themeStorm` host-vs-model lockstep
kit) - impossible if boxing had copied.

One `UiContext*` could NOT survive the change: `fedit`'s post-round-trip worker used to ride
a `void* arg` thread payload. `IUiContext` is a FAT pointer and cannot fit a single word, so
`fedit.cb` / `fedit_jsx.cb` hand the worker its context through a file-scope `IUiContext`
slot set from `activeCtx()` before `Thread.start`. Assertions unchanged.

**`IUiTest`** - `UiTest` is now `class UiTest : IUiTest`. ZERO fields, 46 methods (2 lifecycle
/ 14 actions / 22 readers / `waitUntil` / 7 asserts). The runner still owns a concrete
`UiTest t` and reads its tally (`pass`/`total`/`aborted`/`launched`) directly. New case-body
signature - every UI self-test lambda in `example/ui/**` followed:

```cflat
s.test("save enables after edit", (IUiTest t) => { ... });   // was (UiTest* t)
```

`ui_test.cb` now imports `os.cb` and calls `os.sleep_ms(10)` instead of forward-declaring
`sleep`: `waitUntil` is an `IUiTest` VTABLE SLOT, so it is emitted in EVERY consumer, and the
`sleep` it calls must resolve without a consumer-side `import "time.cb"` (it previously did
not, and every non-`waitUntil` UI example failed to link).

THREE COMPILER FIXES were required - all three are the same shape as Phase 5's bug 0 ("the
direct-call path does X, the other path does not"). Regression cover: test 33
(`testInterfaceArgPaths`) in `Test/test_interface.cb`; the leak is additionally gated by
`example.bat`'s `--heap-audit` UI self-tests.

1. **An INTERFACE-typed parameter of a `function<>` / `Lambda<>` got the raw class value.**
   The indirect-call path (`MainListener.h` [PFX-5]) never boxed a concrete argument into the
   fat pointer the callee expects; the call reached LLVM as `call void %f(%UiT %v)` and failed
   module verification with no source location. It now boxes/upcasts through the new shared
   `LLVMBackend::CoerceArgToInterface` (which also gives a clean error when the argument's
   class does not implement the interface). This is exactly what `function<void(IUiTest)>`
   needs.
2. **A lambda LITERAL argument to an INTERFACE METHOD had no expected signature.** The
   interface-method arg loop never set `lambdaExpectedType`, so the lambda's return type
   defaulted to `void` and `t.waitUntil(() => ..., 2000)` emitted `ret i1` in a void function.
   The loop now reads the declared params from the interface table
   (`LLVMBackend::GetInterfaceMethodParams`), matching the direct path.
3. **An owned-string call RESULT passed as a borrow argument of an INTERFACE METHOD leaked.**
   The direct-call arg loop registers any string-typed `CallInst` argument as an owned string
   temp (freed at end-of-full-expression; `string`'s dtor no-ops on a borrow); the
   interface-method arg loop did not. `t.expectStr("row", "alpha", t.listCellText(...))` leaked
   one buffer per call. Same registration added.

COCOA IS COMPILE-CHECKED ONLY (no Mac box): `cflat.exe cflat/core/ui_native/cocoa.cb --check
--platform macos` passes and it is in the `test_lsp.bat` bulk sweep. Its Phase 6 diff is a
line-for-line mirror of win32 (signature type changes only; no logic touched). It MUST be
validated on Apple Silicon before release.

### Phase 7 - Examples + docs - DONE (2026-07-13)

No compiler and no `core/` change was needed: the whole phase is examples + docs. Gates:
`test.bat` all passed, `test_lsp.bat` 204/204, `example.bat` 89 passed / 0 failed / 24
skipped and LEAK-CLEAN. Cocoa re-checked (`--check --platform macos`, unchanged this phase).

1. **`counter.cb`** - the last concrete-class downcasts in the examples are gone:
   `View* rootView = tree as View` -> `IView rootView = tree as IView`, and
   `rootView.children[1] as Button` -> `as IButton`, each guarded with `== nullptr`. The
   example exists to demonstrate the model, so it now demonstrates the current one.

2. **JSX-result declarations CONVERTED** (`Button* btn = <Button .../>` -> `IButton btn = ...`),
   in `gallery_app.cb`, `todo_app.cb`, `fedit.cb`, `fedit_jsx.cb` (34 declarations).
   Rationale: after Phases 5-6 the concrete pointer was the ONLY place a user still saw a
   class type outside a `<Tag/>`, which made the model read as "factories give interfaces,
   sugar gives pointers" - a distinction with no meaning, since both allocate the same class.
   Both forms compile; consistency wins, and the converted examples now PROVE the JSX
   auto-box in every position it is used: declaration, interface-field write
   (`img.pixels = ...`, `lv.rowText = ...`), nested-struct write through an interface field
   (`grp.style.width = 30`, `split.style.width = 40`), `add()` argument (including a JSX
   child expression `grp.add(<Text .../>)`), a `{expr}` sugar child
   (`fedit_jsx.buildTabs` now returns `move ITabControl`), `return`, and `delete` through the
   fat pointer (`fedit`'s hand-built `IStatusBar sbx`). All leak-clean under `--heap-audit`.
   `ContextMenu*` was deliberately NOT converted (it is not an `IElement`; see the sweep).

3. **`doc/UI.md` rewritten where it was stale** (it documented an API that no longer
   compiles):
   - New section **"Classes construct, interfaces are used"** stating the model, plus the
     `as` / `is` / `== nullptr` downcast idiom and the interface-field lvalue rule.
   - The `IElement` contract block: `paint`/`dispatch` now take `IUiContext`, and the two
     methods the doc never listed (`flexWeight()`, `nodeBounds()`) were added.
   - New **widget-interface hierarchy** block (the `IElement -> ITooltipped -> IDisableable`
     tree) + the implicit-upcast rule.
   - The factory table: all 25 signatures are now `move IX ...` (and `textArea` / `mount`,
     which the table had omitted, are listed).
   - New **"Ownership: who frees the tree"** block (factory owns -> `add` publishes -> tree
     frees).
   - `UiContext` struct block -> the real `interface IUiContext` (+ what stays on the
     concrete class), `render(UiContext* ctx)` -> `render(IUiContext ctx)`,
     `interface IComponent { IElement render(IUiContext ctx); }`,
     `mount` -> `move IElement`.
   - Sugar section: a tag names a CLASS (not the lowercase factory - the old contract text
     was wrong about this), and the result binds to the widget interface.
   - Testing section: case bodies are `(IUiTest t) => {...}`, the API table is `IUiTest`,
     the kit note is `function<void(IUiTest)>`.
   - **BUG in the copy-me template found by the compile gate**: it declared
     `int main(int argc, char** argv)` - no `extern`, which fails to link. Fixed.

4. **`doc/LANGUAGE.md`** - the Phase 1 interface material (fields, `is IFace`, interface
   null-compare, multi-level chains, boxing/ownership) was already there and correct. Two
   coherence fixes only: the interface-field sample now declares the nested struct it uses
   (it referenced an undefined `Rect`), and the inheritance / `is` / `as` samples were
   renamed off `Button`/`IButton` (which collided with the earlier `class Button : IElement`
   in the same section) onto `Press`/`IPress`. No rewrite.

   **Doc samples are compile-gated, not eyeballed.** Three scratch programs were built and
   run against the current compiler: every `doc/UI.md` sample (all 25 factories, the JSX
   forms, `as`/`is`/null-compare, `mount`, `deleteTree`), the doc's two-file test template
   (which is what caught the missing `extern`), and every `doc/LANGUAGE.md` interface sample.
   All three run clean under `--heap-audit`.

5. **Final raw-pointer sweep of `example/ui/**` and `cflat/core/**`.** Every survivor is
   deliberate:

   | Survivor | Why it is legitimately still a pointer |
   |---|---|
   | `list<IElement>* childList()` | pointer-to-LIST, not to an element. Explicitly out of scope (revisit as `childCount()`/`childAt(i)`). |
   | `ContextMenu*` (`buildCtxMenu`/`buildTreeMenu` + the host casts) | `ContextMenu` is NOT an `IElement` - it never enters the tree. It is a menu MODEL the app builds and hands to the host as an opaque `u64` (`nativeSetContextMenu`), which the host then owns and frees. No interface, and none needed. |
   | `GalleryApp* g = activeApp() as GalleryApp;` | a COMPONENT class, not an element. `IComponent` is `render()` only, so reading app model state needs the concrete type. Correct by design (and the doc says so). |
   | `PostBox*` / `ListRowBox*` / `TreeBox*` / `CanvasBox*` | host-seam plumbing: closures boxed to cross the `u64` `INativeHost` seam. Never user-facing. |
   | `View* v = new View();` etc. inside the 24 factory bodies, and `ComponentElement* ce = new ComponentElement();` inside `mount` | the CONSTRUCTION type. The pointer exists for exactly the two statements between `new` and `return`, which is the point of the model. |

   Nothing else remains. Every raw element pointer in user-facing example code is gone.

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

## MAC VALIDATION CHECKLIST (the one thing this refactor could not gate)

`cflat/core/ui_native/cocoa.cb` was touched in Phases 2, 4, 5 and 6 and has been
COMPILE-CHECKED ONLY through all seven phases - there is no Apple Silicon box here. Its
diffs are line-for-line mirrors of the win32 patterns (type changes only; no logic was
touched), but "it compiles" is a weaker claim than the Win32 gates, because the whole
point of this refactor was a change in how VALUES are laid out and dispatched at runtime.
Run this on an arm64 Mac before release:

1. `cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release`
2. `./test.sh Release` - **expect 147 passed / 0 failed** (the Linux/macOS suite baseline).
   This gates the LANGUAGE side of the refactor (`Test/test_interface.cb` carries the
   interface-field, upcast, `is IFace`, null-compare, move-param and dtor-slot cases).
3. A REAL gallery run (not just `--check`):
   `cflat example/ui/05-gallery/gallery.cb -i example/ui --heap-audit -o out/gallery`
   then `./out/gallery --selftest` - expect the 27/27 self-test and NO
   `heap-audit: LEAK` line. This is the only thing that exercises the Cocoa host's
   interface downcasts on live AppKit controls.
4. `cflat example/ui/08-fedit/fedit.cb -i example/ui --heap-audit -o out/fedit` +
   `./out/fedit --selftest` - the multi-widget flagship (tabs / tree / split / context
   menu / `ctx.post`).

What to watch for, in order of likelihood:

- **A wrong-vtable crash at TEARDOWN, not at use.** This is Phase 5's bug 0 signature: a
  derived-interface value stored in a parent-interface slot still dispatches METHODS
  correctly (a parent's methods are a prefix of the child's flattened vtable), so the UI
  looks perfect and only `delete c` in `destroyTree()` faults - jumping to a small integer
  address (a field BYTE OFFSET read as a function pointer, e.g. 0x18). If Cocoa faults on
  window close with a tiny fault address, this is it.
- **A missing interface field on a Cocoa-only reach.** `ScrollView.contentH` is read only
  by the Cocoa host; it IS on `IScrollView`, but it is the one field no Windows gate
  exercises.
- **`_syncButtonAccent(u64, IButton)`** - Cocoa's only host-internal helper that has no
  win32 twin; it is a BORROW param (plain `IFace`, not `move`), so do not "fix" it by
  adding `move`.
- **Leaks through the boxed closures** (`IListView.rowText`, `ITreeView.childCount`/
  `childId`/`label`, `ICanvasView.onPaint`): reading a `Lambda` interface field BY VALUE
  clones it. Win32 proves this is leak-neutral; the Cocoa `NSTableView` dataSource /
  `NSOutlineView` paths box the same closures, so `--heap-audit` on the gallery is the check.
