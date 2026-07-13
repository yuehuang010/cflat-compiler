# UI framework: pointers -> interfaces

Status: PLANNING (gated on the F1 feasibility spike - see
`internal/plan/interface-fields-feasibility.md`)
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

If F1 comes back NOT FEASIBLE, the fallback is accessor methods on every
interface (~300 mechanical call-site rewrites, clumsy nested-struct handling).
That decision gates everything below.

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

### Phase 1 - Language (gated on the spike)
Implement F1-F4 with regression coverage EXTENDED INTO `Test/test_interface.cb`
(do not create new test files). Error tests for the new diagnostics go in
`Test/errors/` (e.g. a class missing an interface field; a field type mismatch).
Gate: `test.bat` + `test_lsp.bat` green, UI framework untouched.

### Phase 2 - Rename (mechanical, no semantics)
`Element`->`IElement`, `Canvas`->`ICanvas`, `Component`->`IComponent`,
`NativeHost`->`INativeHost`, across `core/ui_native.cb`, `core/ui_native/*.cb`,
`core/ui_canvas/*.cb`, `core/ui_test.cb`, `example/ui/**`, `doc/UI.md`.
Gate: all three suites green. Ship separately - a rename that is tangled with a
semantic change is unreviewable.

### Phase 3 - Widget interfaces
Declare `interface IView : IElement`, `IButton : IElement`, ... for all 23
element classes, carrying the fields each host/reconciler actually reaches
(derive the list from the measured 255 reach-throughs; ~45 distinct fields).
Convert the framework internals: the 23 `propsEqual` downcasts and the layout
flex probes (`ui_native.cb:814`, `:821`, `:850` - these reach a NESTED struct
field, `v.style.width`).

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
