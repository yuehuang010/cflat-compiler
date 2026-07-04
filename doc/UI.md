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

- **Framework API version:** 9 (v5 added color to `Style` + `Canvas`; v6 added the
  `Theme` styling preference and hover/pressed/focus interaction states; v7 added the
  `Checkbox` widget, the `disabled` state, and `Style.gap`; v8 added the `ProgressBar`
  and `Slider` widgets with the `track`/`accent` theme slots; v9 added the `NativeHost`
  seam - real OS controls as an alternative output stage to `Canvas` - plus `Patch.key`
  identity, the `UiContext.nativeByKey` shadow map, and the DIP layout unit; v10 added
  the `TextArea` multiline element and the Win32 native host's editor features -
  per-monitor DPI, dark titlebar + themed controls, a declarative menu bar with
  accelerators, shell file dialogs, message boxes, and a secondary window)
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
- `toJson()` returns a `move string` (owned). Using its result inline -
  `printf("%s", tree.toJson().data())` - is leak-clean: the owned virtual-call temp
  is freed at end-of-expression, just like a free-function result. Binding to a
  local (`string j = tree.toJson();`) is equally fine.
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
| `Button` | `ELEM_BUTTON` | pressable leaf | `string title`, `Lambda<void()> onPress`, `bool disabled` |
| `Box` | `ELEM_BOX` | bordered sized container | `Style style`, `children` |
| `TextInput` | `ELEM_TEXTINPUT` | controlled text field | `string value`, `Lambda<void(string)> onChangeText`, `int width`, `bool disabled` |
| `Checkbox` | `ELEM_CHECKBOX` | controlled on/off toggle | `string label`, `bool checked`, `Lambda<void(bool)> onChange`, `bool disabled` |
| `ProgressBar` | `ELEM_PROGRESS` | presentational fill bar | `int value` (0..100), `int width` |
| `Slider` | `ELEM_SLIDER` | controlled value via click/drag/arrows | `int value` (0..100), `int width`, `Lambda<void(int)> onChange`, `bool disabled` |
| `TextArea` | `ELEM_TEXTAREA` | multiline text region (editor) | `string value` (push), `Lambda<void()> onChange` (dirty notify), `int width/height` |
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
Checkbox*    checkbox(string label, bool checked, Lambda<void(bool)> onChange);
ProgressBar* progressBar(int value, int width);
Slider*      slider(int value, int width, Lambda<void(int)> onChange);
ScrollView*  scrollView(Style style);
```

**Controlled widgets** (`TextInput`, `Checkbox`) hold no state of their own: the
component owns the value and passes it down each render; the widget fires its
`on*` handler with the new value and the component updates state + `invalidate()`s,
so the next render flows it back. **`disabled`** dims the widget (`dimColor`) and
makes it ignore input and drop out of the Tab focus ring.

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
    int flexDirection = 0;       // DIR_COLUMN (0) | DIR_ROW (1)
    int color = 0;               // foreground (text / border); COLOR_DEFAULT (0) = default ink
    int backgroundColor = 0;     // fill; COLOR_DEFAULT (0) = no fill
    int gap = 0;                 // cells of space inserted between stacked children
};
Style makeStyle(int padding, int width, int height);   // flexDirection column, colors default
int   rgb(int r, int g, int b);                        // build an explicit color
int   COLOR_DEFAULT;                                   // 0: backend monochrome default
```

ALWAYS build colors with `rgb(r, g, b)` - never a raw `0xRRGGBB` literal. `rgb()`
sets a marker bit so an explicit color is non-zero (even `rgb(0,0,0)`), and
`COLOR_DEFAULT` is `0` ("no explicit color"): a tree that sets no color renders
exactly as before colors existed. 0-as-sentinel is deliberate so a default/zeroed
field is automatically "unset" - the framework does not rely on a `-1` initializer
surviving (a nested `Struct f = default;` field is zero-filled in CFlat). The GDI
host masks the marker off when converting to a `COLORREF`.

`color`/`backgroundColor` live on `Style` (read by `View`/`Box`/`ScrollView`) and
as direct fields on the color-bearing leaves: `Text.color`, `Button.color` +
`Button.backgroundColor`, `TextInput.color` + `TextInput.backgroundColor`. A
backend without color (the TUI char grid) ignores them.

### Theme (styling preference)

Rather than color every node by hand, set a `Theme` once on the context and widgets
that specify no color of their own inherit it. An explicit per-node color still wins.

```
struct Theme {
    int textColor; int panelBg; int panelBorder;
    int buttonBg;  int buttonText;
    int inputBg;   int inputText;
    int focusRing;
    int track;     int accent;             // all COLOR_DEFAULT (0) in a default Theme
};
Theme lightTheme();   // slate-on-near-white, blue primary button
Theme darkTheme();    // light-on-charcoal, brighter blue button
int   pickColor(int nodeColor, int themeColor);   // node color wins, else theme slot
int   shade(int color, int delta);                // lighten/darken (hover/pressed)
```

The theme lives on `UiContext` (`ctx.theme`), the host-owned per-app state. An app
declares its preference in `render()`:

```
Element render(UiContext* ctx) {
    ctx.theme = lightTheme();   // or darkTheme(), or your own Theme
    ...
}
```

A default-constructed `Theme` is monochrome (every slot `0`), so a host that sets no
theme renders exactly as the pre-color framework did - this is what keeps the GDI
self-test and the TUI hosts byte-identical. `Box`/`Text`/`Button`/`TextInput`
resolve each color as `pickColor(nodeColor, themeSlot)`; `View`/`ScrollView` fill
only when given an explicit `style.backgroundColor`.

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
int EV_MOUSE_MOVE = 3;   // hit-tested; updates hover (paint-only state)
int EV_MOUSE_UP   = 4;   // hit-tested; host also clears pressed on release
int KEY_ENTER = 13; int KEY_SPACE = 32; int KEY_TAB = 9;   // VK codes match these

struct Event { int kind; int x; int y; int key; };
Event mouseDown(int x, int y);
Event mouseMove(int x, int y);
Event mouseUp(int x, int y);
Event keyPress(int key);
```

- **Pointer events** hit-test: containers recurse, a leaf consumes the event if
  the point is within its `bounds`.
- **Key events** route to the FOCUSED node. Focus is a KEYED IDENTITY held in
  `UiContext.focusKey`, so it survives reconcile (a re-render keeps the same key
  -> same focus), unlike a list index. A `Button` takes focus on press and, while
  focused, re-fires on Enter/Space.

### Interaction visual states (hover / pressed / focus)

`hover` and `pressed` are KEYED identities in `UiContext` (like focus), set as the
pointer moves/presses and rendered as a shade of the widget's color. They are PURE
PAINT STATE: changing them calls `requestRepaint()` (no re-render), and only when the
identity actually changes, so dragging within one widget does not churn.

- A `Button` lightens its fill on hover (`shade(bg, +28)`), darkens it while pressed
  (`shade(bg, -40)`; pressed wins over hover), and draws a `theme.focusRing` border
  when focused. A monochrome (unset) button shows no hover/press fill - `shade(0,..)`
  stays `0` - so the default look is unchanged.
- The host maps native input: `WM_MOUSEMOVE -> mouseMove` (a move hitting nothing
  calls `ctx.setHover("")`), `WM_LBUTTONUP -> mouseUp` + `ctx.clearPress()` (so a
  release off-target still clears). `int shade(int color, int delta)` lightens/darkens
  every channel (clamped); an unset color stays unset.

## `UiContext` (host-owned runtime state)

```
struct UiContext
{
    bool   dirty;       // a handler changed state; host re-renders
    string focusKey;    // keyed identity of the focused node ("" = none)
    string hoverKey;    // node under the pointer ("" = none)
    string pressKey;    // node pressed/held ("" = none)
    Theme  theme;       // styling preference (default = monochrome)
    dictionary<string, u64> nativeByKey;   // v9: key path -> native control handle

    void invalidate();          // mark the UI dirty
    bool consumeDirty();        // read + clear the flag
    void focus(string key);     // set focus to a keyed node
    void blur();
    bool hasFocus(string key);
    void setHover(string key);  // repaints only when hover identity changes
    bool hasHover(string key);
    void setPress(string key);
    void clearPress();
    bool hasPress(string key);

    void setNativeHandle(string key, u64 h);   // v9: shadow-map accessors
    u64  nativeHandle(string key);             // 0 if none
    bool hasNativeHandle(string key);
    void removeNativeHandle(string key);
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
struct Patch { int op; int nodeKind; string detail; string key; };
```

`Patch.key` (v9) is the affected node's stable key path - the parent chain of node
keys joined with `/`, with unkeyed nodes carrying a synthesized positional segment
(`#i`). It lets a `NativeHost` (below) route a create/update/destroy to the right
control by identity. Adding it did not change patch op/kind/count/order, so the
Canvas self-tests are byte-identical.

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
    void drawText(int col, int row, const char* s, int color);  // color via rgb(), 0 = default
    void drawRect(Rect r, bool filled, int color);              // filled=false: border; true: fill
};
```

- **TUI:** `SurfaceCanvas` adapts a headless `Surface` char grid; `canvasFor(&surface)`
  binds one for a paint call. The grid has no color, so it ignores the `color` arg.
- **Win32:** `GdiCanvas` (in `win32host.cb`) maps a cell to `CELL_W`x`CELL_H`
  pixels and draws with `DrawTextA`/`FrameRect`/`FillRect`. It honors `color`
  (`0`/COLOR_DEFAULT keeps the original black ink / black border path) and the host selects a
  crisp monospace font (`createUiFont()`, Consolas) into the DC before painting.
  Layout stays cell-based, so the font is monospace to keep text aligned to box
  borders and the caret; proportional/pixel layout remains deferred.

### Viewing the rendered UI (`win32_shot.cb`)

A headless build harness has no display, so the GDI output is captured to an image
instead of shown in a window: `example/ui/win32_shot.cb` paints the tree into an
offscreen memory DC through `GdiCanvas`, reads the pixels back, and writes a 24bpp
BMP via `core/graphic/bitmap.cb`. Run `out\win32_shot.exe <path.bmp>` to produce a
viewable snapshot of exactly what `GdiCanvas` draws - a real visual oracle beyond
the single-pixel `guiSelfTest` readback.

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
- An owned-string expression used directly as a child attribute next to a sibling
  capturing closure (e.g. `<Text text={"Count: " + n.toString()} />` beside
  `<Button onPress={() => {...}} />`) is leak-clean - the intermediate temp is
  freed by the outer flush, which a nested closure invoker no longer clobbers. No
  bind-to-local workaround is needed; `counter_jsx.cb` exercises this exact shape
  under example.bat's `--heap-audit` gate.

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

## The `NativeHost` seam (v9) - real OS controls

`Canvas` paints the tree pixel by pixel. `NativeHost` is the alternative output
stage: instead of painting, it drives real OS controls (HWND on Windows; NSView
later) so the OS handles rendering, text input, IME, and accessibility. The
app-facing model (Element tree, Component, controlled widgets, Theme, keyed
identity) is unchanged - only the output stage differs. The contract lives in
`example/ui/ui_native.cb`; the Win32 implementation is `win32_native_host.cb`.

```
interface NativeHost
{
    u64  createControl(int elemKind, u64 parentHandle, string key);
    void destroyControl(u64 h);
    void setFrame(u64 h, Rect frameDip);
    void setText(u64 h, const char* s);
    void setBoolProp(u64 h, int prop, bool v);      // PROP_CHECKED/ENABLED/VISIBLE
    void setIntProp(u64 h, int prop, int v);        // PROP_VALUE/MAX/CARET/SCROLLY
    void setAccent(u64 h, int fgColor, int bgColor);// 0 = native default
    Size measureText(const char* s, int fontId, int wrapWidthDip);  // FONT_UI/FONT_MONO
    void requestLayout();
};
```

Design rules that keep the seam WinUI-3-ready: handles are opaque `u64` and are
never dereferenced by `ui.cb`; every mutation is a property set keyed by
`(handle, prop)` (a 1:1 map to a XAML `DependencyProperty` set later); events flow
back only through the existing `Event` + `dispatch()` seam, with the host owning
the native->`Event` translation.

- **Shadow map.** `UiContext` gains `dictionary<string, u64> nativeByKey` mapping a
  node's key path to its control handle, with accessors `setNativeHandle` /
  `nativeHandle` / `hasNativeHandle` / `removeNativeHandle`. Only a `NativeHost`
  touches it; Canvas hosts leave it empty and inert. The handles are non-owning
  integers, so tearing the map down destroys no windows - the host destroys
  controls explicitly.
- **DIP units.** Layout math in `ui.cb` is unit-blind integer arithmetic. A
  Canvas/TUI host reads those integers as cells (1 DIP == 1 cell, so the TUI stays
  byte-identical); a `NativeHost` reads them as DIPs and scales DIP->physical px at
  the host boundary using the monitor DPI. `measureText` replaces the
  1-cell-per-char assumption for native text.

### Win32 native host: editor features (v10)

`example/ui/win32_native_host.cb` (the `Win32Host` implementation of `NativeHost`)
adds the pieces a real desktop app needs. All are documented-API only (no uxtheme
ordinals). An app imports this host and calls `runAppGuiNative(new App(), title)`.

- **Modern look:** per-monitor-v2 DPI awareness with DIP->px scaling and
  `WM_DPICHANGED` relayout; Segoe UI Variable font; immersive dark-mode titlebar +
  themed control/window colors driven by the app `Theme` (`WM_CTLCOLOR*` +
  `WM_ERASEBKGND`). The menu bar and stock scrollbars stay light in dark mode
  (documented-API limit).
- **`TextArea`** is a multiline native `EDIT` and is **uncontrolled-with-sync** (a
  deliberate exception to the controlled-widget rule for large buffers): the native
  buffer is the source of truth, so typing never round-trips through the framework.
  `value` is pushed only at creation; `onChange` is a dirty NOTIFICATION. Pull/push
  the text with the host helpers `nativeControlText(keyPath)` /
  `nativeSetControlText(keyPath, s)`.
- **Menu bar + accelerators:** `menuReset()`, `menuAddTop(title)`,
  `menuAddItem(top, label, accel, cmdId)`, `menuAddSeparator(top)` build the menu;
  `setMenuHandler(Lambda<void(int)>)` receives command ids (menu clicks AND
  accelerators like `"Ctrl+S"` / `"F3"`).
- **Dialogs:** `nativeOpenFile()` / `nativeSaveFile()` (shell file dialogs),
  `nativeConfirm(title, text)` (Yes/No/Cancel -> 1/0/-1), `nativeInfo(title, text)`.
- **Windowing:** `setCloseQuery(Lambda<bool()>)` vetoes window close (dirty-save
  prompt); `showSecondaryWindow(title, message)` opens a real second top-level
  window; `nativeCloseWindow()` quits.
- **Flagship demo:** `example/ui/fedit/fedit.cb` - a small native text editor built
  entirely on the above (open/edit/save, find, dirty-close prompt, light/dark, two
  windows). Build: `cflat example/ui/fedit/fedit.cb -i example/ui -o out/fedit.exe`.

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
