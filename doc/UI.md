# UI Framework (v0.4)

A small React-Native-style declarative UI library for CFlat, living in
`example/ui/ui.cb`. It builds an `Element` tree, diffs renders with a keyed
reconciler, routes input through one `dispatch(Event)` seam, lays out in a single
constraints-in/size-out pass, and paints through a surface-agnostic `Canvas` so
the same tree renders to a terminal (TUI) or a native Win32 GDI window.

This document is the FROZEN CONTRACT for v0.4: the surface below is stable, and
the "sugar-compatible element contract" is what a future `<View/>` grammar sugar
(and an eventual `core/` promotion) desugars against. Changes to the frozen
surface are deliberate and bump the API version.

- **Framework API version:** 4
- **Location:** `example/ui/` (framework + TUI and Win32 hosts). NOT in `core/`
  yet - promotion is deferred to a later release.
- **Status:** the framework is a single source of truth (`example/ui/ui.cb`);
  every host (TUI and the Win32 `win32host.cb`) imports it directly.

## Constraints (read first)

- **Single mounted app, single thread.** One `UiContext` per host loop drives one
  mounted tree. The framework library carries NO process-global mutable state;
  all per-app runtime state (the dirty flag and the focused-node identity) lives
  in a host-owned `UiContext` that is threaded into `render()`. The Win32 host
  keeps its window singletons (`_gApp`/`_gTree`/`_gCtx`/...) in the HOST file, not
  in the library, because a stdcall `WndProc` cannot capture `this`.
- **Cell units.** Layout and paint are in character CELLS. The `Canvas`
  implementation owns the cell->pixel mapping (1:1 for the TUI; `CELL_W`/`CELL_H`
  for Win32 GDI).
- **No downcast in the hot path.** The reconciler compares node types via
  `kind()`, never an `as` downcast. `propsEqual()` may downcast `other` because
  the reconciler guarantees both nodes share a `kind()` first.

## The `Element` contract

Every node implements `Element`. A node type is sugar-compatible (see below) when
it implements this interface AND exposes the construction shape described later.

```
interface Element
{
    move string toJson();                 // serialize self + subtree
    void destroyTree();                   // free owning descendants (not self)
    Size layout(LayoutConstraints c);     // place self, return consumed size
    void paint(Canvas c, UiContext* ctx); // draw self + subtree (focus-aware) via Canvas
    bool dispatch(Event e, UiContext* ctx); // route one input event; true if consumed
    int  kind();                          // ELEM_* tag for the reconciler
    bool propsEqual(Element other);       // structured props compare (same kind)
    list<Element>* childList();           // children for containers, nullptr for leaves
    string keyOf();                       // "" if unkeyed; drives keyed reconciliation
    void setKey(string k);
    void collectFocusables(list<string>* keys); // append focusable keys (Tab ring)
};
```

Notes:
- `toJson()` returns a `move string` (owned). Bind it to a local before using
  `.data()` - e.g. `string j = tree.toJson(); printf("%s", j.data());` - so the
  buffer is freed at scope exit. (Inline `.data()` on the virtual result currently
  leaks; see `internal/issue/virtual-move-string-result-inline-use-leak.md`.)
- `destroyTree()` frees a node's owning DESCENDANTS only; the node itself is freed
  by its owner. Use the free helper `deleteTree(move Element root)` to tear down a
  whole tree (descendants then root), and `delete` dispatches virtually.
- Leaves return `nullptr` from `childList()`; the reconciler uses that to choose
  the leaf-compare path (`propsEqual`) vs the child-diff path.

## Built-in nodes

| Node | Kind | Role | Key props |
|------|------|------|-----------|
| `View` | `ELEM_VIEW` | flex container | `Style style`, `list<Element> children` |
| `Text` | `ELEM_TEXT` | text leaf | `string text` |
| `Button` | `ELEM_BUTTON` | pressable leaf | `string title`, `Lambda<void()> onPress` |
| `Box` | `ELEM_BOX` | bordered sized container | `Style style`, `children` |
| `TextInput` | `ELEM_TEXTINPUT` | controlled text field | `string value`, `Lambda<void(string)> onChangeText`, `int width` |
| `ScrollView` | `ELEM_SCROLL` | clipped scrollable viewport | `Style style` (viewport), `children`, `int scrollY` |
| `ComponentElement` | `ELEM_COMPONENT` | wraps a mounted component subtree | single inner child |

Convenience constructors return the concrete heap pointer so callers can set
typed props, then assign into an `Element` slot:

```
View*       view(Style style);
View*       row(Style style);      // View with flexDirection = DIR_ROW
View*       column(Style style);   // View with flexDirection = DIR_COLUMN
Text*       text(string s);
Button*     button(string title, Lambda<void()> onPress);
Box*        box(Style style);
TextInput*  textInput(string value, Lambda<void(string)> onChangeText);
ScrollView* scrollView(Style style);
```

### TextInput, ScrollView, focus navigation

- **TextInput** is CONTROLLED: the component owns the text and passes it down as
  `value` each render; on a keystroke the field fires `onChangeText(newValue)` and
  the component updates state + `invalidate()`, so the next render flows the new
  value back. A focused field inserts printable keys, deletes on `KEY_BACKSPACE`,
  and paints a trailing `_` caret. The per-keystroke buffer is an owning `string`.
- **ScrollView** lays its children in a tall virtual column shifted by `scrollY`
  and paints them clipped to its viewport via `pushClip`/`popClip`. A focused
  ScrollView scrolls on Up/Down. Scrolling changes layout but not the tree, so it
  calls `ctx.requestRepaint()` (the host repaints without a re-render - reconcile
  would otherwise produce no patches).
- **Tab navigation:** `void collectFocusables(list<string>* keys)` walks the tree
  collecting focusable node keys in order; `moveFocus(tree, ctx, backward)` builds
  that ring fresh each call and advances focus with wraparound. The host calls it
  on Tab / Shift-Tab. Built from the live tree, focus tracks keyed identity across
  re-renders.

### Canvas clip

`pushClip(Rect)` / `popClip()` bound subsequent drawing to a rectangle (cells);
nested pushes intersect, and calls must be balanced. The TUI `SurfaceCanvas` keeps
a fixed inline clip stack (no heap, so the interface-temporary value needs no
destructor); the Win32 `GdiCanvas` uses `SaveDC` + `IntersectClipRect` / `RestoreDC`.

## Style (React-Native field names)

`Style` is a TYPED struct, not an open dynamic bag - the one deliberate deviation
from RN, which takes arbitrary string keys. Fields track RN names:

```
struct Style
{
    int padding = 0;
    int width = 0;
    int height = 0;
    int flexDirection = 0;   // DIR_COLUMN (0) | DIR_ROW (1)
};
Style makeStyle(int padding, int width, int height);   // flexDirection defaults to column
```

## Layout protocol (one pass, pluggable strategy)

Layout is a single constraints-in/size-out pass. A node lays out within a
`LayoutConstraints` and returns the `Size` it consumed, stamping its absolute
`bounds` so paint and hit-test agree. Flex/row/column/scroll are STRATEGIES over
this one pass, not bespoke per-node math.

```
struct LayoutConstraints { int x; int y; int availW; int availH; };
struct Size              { int w; int h; };
Size layoutRoot(Element root, int x, int y, int w);   // root pass, height unbounded
```

`View` reads `style.flexDirection`: `DIR_COLUMN` stacks children top-to-bottom,
`DIR_ROW` places them left-to-right. New layout strategies are added here, and
new widgets plug into the same pass rather than bolting on.

## Input: the `dispatch(Event)` seam and focus

All input flows through one path. The host translates native input into an
`Event` and calls `tree.dispatch(e, ctx)`.

```
int EV_MOUSE_DOWN = 1;   // hit-tested down the tree
int EV_KEY        = 2;   // routed to the focused node
int KEY_ENTER = 13; int KEY_SPACE = 32; int KEY_TAB = 9;   // VK codes match these

struct Event { int kind; int x; int y; int key; };
Event mouseDown(int x, int y);
Event keyPress(int key);
```

- **Pointer events** hit-test: containers recurse, a leaf consumes the event if
  the point is within its `bounds`.
- **Key events** route to the FOCUSED node. Focus is a KEYED IDENTITY held in
  `UiContext.focusKey`, so it survives reconcile (a re-render keeps the same key
  -> same focus), unlike a list index. A `Button` takes focus on press and, while
  focused, re-fires on Enter/Space.

## `UiContext` (host-owned runtime state)

```
struct UiContext
{
    bool   dirty;       // a handler changed state; host re-renders
    string focusKey;    // keyed identity of the focused node ("" = none)

    void invalidate();          // mark the UI dirty
    bool consumeDirty();        // read + clear the flag
    void focus(string key);     // set focus to a keyed node
    void blur();
    bool hasFocus(string key);
};
```

The host owns one `UiContext`, threads `&ctx` into `render()`, and drains
`consumeDirty()` once per loop, re-rendering + reconciling only on a change.
Event closures call `ctx.invalidate()`.

## Components and the model layer

```
interface Component { Element render(UiContext* ctx); };
ComponentElement* mount(Component c, string key, UiContext* ctx);
```

A component renders itself to an `Element` tree; `render` receives the host
`UiContext` so its event closures can `invalidate()`. A parent component owns its
children by pointer (e.g. `list<Counter*>`) and re-mounts them by key each render;
the reconciler matches `ComponentElement`s by key and descends into the single
inner child, so each child's subtree and state keep identity across
add/remove/reorder.

## Reconciler and patches

`reconcile(move committed, move wip, list<Patch>*)` diffs a freshly-rendered WIP
tree into the committed (live) tree in place, recording changes as patches and
freeing whichever subtree it discards. Unchanged nodes keep identity; changed
leaves are swapped in from WIP.

```
int PATCH_UPDATE; int PATCH_REPLACE; int PATCH_INSERT; int PATCH_REMOVE;
struct Patch { int op; int nodeKind; string detail; };
```

Leaf change-detection is the structured `propsEqual()` - no stringly-typed
signatures. Children are matched by key when keyed (so middle insert/remove and
reorder preserve the right nodes) and positionally otherwise. The patch log is
in-memory only; a serialized wire format / OTA is out of scope.

## The `Canvas` seam

Nodes paint via `Canvas` primitives in cell coordinates, never poking a surface
directly, so the same tree renders to any backend.

```
interface Canvas
{
    void clear();
    void drawText(int col, int row, const char* s);
    void drawRect(Rect r, bool filled);   // filled=false: border; true: fill
};
```

- **TUI:** `SurfaceCanvas` adapts a headless `Surface` char grid; `canvasFor(&surface)`
  binds one for a paint call.
- **Win32:** `GdiCanvas` (in `win32host.cb`) maps a cell to `CELL_W`x`CELL_H`
  pixels and draws with `DrawTextA`/`FrameRect`/`FillRect`.

## `<View/>` grammar sugar

JSX-like element syntax (Phase 2) that lowers 1:1 to construction + children-add,
so it is interchangeable with the explicit constructor form. It is
LIBRARY-AGNOSTIC: `<Tag>` constructs whatever type named `Tag` is in scope (the
compiler has no dependency on this library), sets attributes as fields, and adds
children via the tag's `add()` method.

```
return <View style={makeStyle(16, 0, 0)}>
         <Text text={label} />
         <Button title="+1" onPress={() => { this.count = this.count + 1; }} />
       </View>;
```

- `attr="literal"` sets a string field from a static literal; `attr={expr}` sets
  any field from an expression (including `onPress={() => {...}}` lambdas).
- Children are nested elements or `{expr}` interpolations; each is `add()`-ed to
  the parent, so the tag needs an `add(Element)` method (View/Box/ScrollView have
  one; leaves like Text do not take children).
- The result is a heap node pointer usable wherever a constructor result is - e.g.
  `return <View/>` or assigning to an `Element` slot.
- `counter_jsx.cb` asserts the sugar desugars to a tree identical to the
  hand-written `counter.cb` form.

Gotcha: an owned-string expression used directly as a child attribute next to a
sibling capturing closure (e.g. `<Text text={"Count: " + n.toString()} />` beside
`<Button onPress={() => {...}} />`) leaks the intermediate string temp - a known
temp-flush limitation (`internal/issue/element-sibling-temp-flush-leak.md`). Bind
the string to a local first (`string label = ...; <Text text={label} />`); this is
the leak-clean idiom and reads clearly besides.

### Sugar-compatible element contract

The sugar lowers 1:1 to plain construction calls and
resolves against whatever node type named `View`/`Text`/... is in scope (the
compiler stays ignorant of this library). For a node type to be a valid sugar
target it MUST expose:

1. A constructor reachable by the tag name - either a `T* tag(...)` factory (as
   `view`/`text`/`button`/`box` provide) or a brace-init shape - so `<View .../>`
   lowers to constructing a `View`.
2. A children add path: a `void add(Element child)` method backed by a
   `list<Element>` (containers), so nested children lower to `parent.add(child)`.
3. Prop fields settable after construction (`style`, `text`, `title`, `onPress`,
   `key` via `setKey`), so `prop="literal"` / `prop={expr}` attributes lower to
   field assignments / setter calls.
4. The full `Element` interface (above), so the lowered tree reconciles, lays out,
   paints, and dispatches uniformly.

Keeping this contract stable is what lets the sugar work unchanged if the library
later moves to `core/`.

## React-Native naming convention

Names mirror React Native so an RN developer feels at home. This constrains the
NAMES of the surface that already exists; it does NOT add JS semantics, hooks, a
runtime, or Yoga.

| Concept | This framework | RN |
|---------|----------------|----|
| Container | `View` (+ `flexDirection`) | `View` |
| Text | `Text` | `Text` |
| Pressable | `Button` (`title`, `onPress`) | `Button`/`Pressable` |
| Press event | `onPress` | `onPress` |
| Style | typed `Style` (`flex*`, `padding`, `width`, `height`) | style object |
| Bordered box | `Box` | (no direct RN analog) |

`Row`/`Column`, `TextInput`, `ScrollView`, and Tab focus navigation are
implemented as thin layers over these seams (see above), not special cases.

## Hosts

- **TUI** (`example/ui/host.cb`, `example/ui/boxes.cb`): a double-buffered
  terminal host. Real console = interactive; redirected I/O (`--run`, CI) falls
  back to a deterministic headless self-test that gates `example.bat` on behavior
  AND leaks (built with `--heap-audit`).
- **Win32** (`example/ui/win32host.cb` + `example/ui/win32_boxes.cb`): a native
  GDI host reusing the framework unchanged behind the `Canvas` seam.

To run the TUI self-tests directly:

```
x64/Release/cflat.exe example/ui/host.cb --run
x64/Release/cflat.exe example/ui/boxes.cb --run
```
