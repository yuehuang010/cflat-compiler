# ui_native - native UI framework

`ui_native` is a small React-Native-style declarative UI library for CFlat,
shipped as a core module (`core/ui_native.cb`). It builds an `Element` tree, diffs
renders with a keyed reconciler, routes input through one `dispatch(Event)` seam,
lays out in a single constraints-in/size-out pass, and paints through a
surface-agnostic `Canvas` (TUI / Win32 GDI) or drives real OS controls through the
`NativeHost` seam (Win32, macOS AppKit, WinUI 3).

The framework ships with the compiler and is versioned with it - there is no
independent framework version number. The "sugar-compatible element contract" below
is what the `<View/>` grammar sugar desugars against; changes to that surface are
deliberate. The element/seam surface was frozen in the pre-promotion hardening pass
(native accessibility Names, the keyboard/focus contract, and the gallery theme-flip
+ reconcile stress-soak self-tests) and then promoted into `core/` unchanged.

- **Location:** `core/` - the framework and every host deploy next to the compiler,
  so apps import them with no `-i`:
  - `core/ui_native.cb` - the framework + the `NativeHost` seam (the one module apps import
    for the Canvas hosts; the Element tree, reconciler, `Theme`, `Canvas`, and the seam
    interface + `PROP_*`/`FONT_*`/`LISTOP_*` consts all live here).
  - `core/ui_native_host.cb` - the `if const` platform shim: selects the Win32 host on
    Windows, the Cocoa host on macOS. Apps that want real OS controls import this.
  - `core/ui_native_win32.cb` - the Win32/GDI `NativeHost` backend.
  - `core/ui_native_cocoa.cb` - the macOS AppKit (Cocoa) `NativeHost` backend (imports
    `core/cocoa.cb`, the objc bridge); compile-verified only (see the parity matrix).
  - `core/ui_native_winui.cb` - the WinUI 3 (Windows App SDK) `NativeHost` backend.
- **Status:** the framework is a single source of truth (`core/ui_native.cb`); every
  host imports it directly. See the parity matrix below for per-host coverage and the
  documented WinUI 3 / Cocoa gaps.

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
| `StatusBar` | `ELEM_STATUSBAR` | bottom strip of text panes (app shell) | `list<string> parts` (pane text) |
| `RadioGroup` | `ELEM_RADIOGROUP` | controlled single-choice group (container) | `int value` (selected index), `onChange`, `RadioButton` children |
| `RadioButton` | `ELEM_RADIO` | one option inside a `RadioGroup` | `string label`, `int index`, `bool checked` |
| `ComboBox` | `ELEM_COMBO` | controlled dropdown | `list<string> items`, `int selectedIndex`, `onChange` |
| `ListView` | `ELEM_LIST` | virtualized report-mode list | `columns`, `int rowCount`, `rowText(row,col)`, `int selectedIndex`, `onSelect`, `onActivate` |
| `TabControl` | `ELEM_TABS` | keyed tab panes, lazy inactive tabs | `int selectedTab`, `onSelectTab`, `TabPane` children |
| `TabPane` | `ELEM_TABPANE` | one titled pane inside a `TabControl` (container) | `string title`, `children` |
| `TreeView` | `ELEM_TREE` | expand-on-demand tree | `childCount(nid)`, `childId(nid,i)`, `label(nid)`, `int selectedNode`, `onSelect`, `onExpand` |
| `SplitView` | `ELEM_SPLIT` | two weighted panes + draggable divider (container) | `int ratio`, `bool vertical`, `onRatioChange`, two pane children |
| `Image` | `ELEM_IMAGE` | bitmap leaf (BGRA32 via `setImageData`) | `u64 pixels` (borrowed top-down BGRA32), `int pxW/pxH`, `int width/height`, `string source` (.bmp), `string altText` |
| `GroupBox` | `ELEM_GROUPBOX` | titled group frame (container) | `string title`, `Style style`, `children` |
| `CanvasView` | `ELEM_CANVAS` | app-painted escape hatch | `Lambda<void(Canvas)> onPaint`, `int width/height` |
| `ScrollView` | `ELEM_SCROLL` | clipped scrollable viewport | `Style style` (viewport), `children`, `int scrollY` |
| `ComponentElement` | `ELEM_COMPONENT` | wraps a mounted component subtree | single inner child |

**`tooltip` prop (v12):** every mappable element (`Text`/`Button`/`Box`/`TextInput`/
`Checkbox`/`ProgressBar`/`Slider`/`TextArea`/`RadioButton`/`ComboBox`/`ListView`/`TabControl`/`TreeView`) carries a `string tooltip` field ("" = none).
A native host attaches it to the OS control (Win32 `tooltips_class32`, Cocoa `setToolTip:`);
Canvas hosts ignore it. Use a string literal (a bare `string` field does not auto-free).

**`StatusBar` (v12):** an app-shell strip whose `parts` list holds one string per pane
(`statusBar(mainText)` + `addPart(...)`). The Win32 host maps it to a real
`msctls_statusbar32` (which self-docks at the window bottom); Cocoa uses a read-only
`NSTextField` strip; a Canvas host draws one `" | "`-joined text row. Read a pane back
with `nativeStatusText(keyPath)`.

**Data controls (v13).** Three controlled tier-1 data controls, each with a Canvas
fallback and a headless self-test in `example/ui/gallery/gallery.cb`:

- **`RadioGroup` / `RadioButton`** - a single-choice group. `RadioGroup` is a layout
  container (like `View`) owning `int value` (the selected index) + `onChange(index)`;
  its `RadioButton` children each carry `label`/`index`/`checked` (controlled: seed
  `checked` from `value`, e.g. `checked={value == i}`, or use `group.addOption(label)`
  which wires it). Win32 maps each radio to a `BS_AUTORADIOBUTTON` (WS_GROUP on the
  first); a click routes to the group's `onChange`. Canvas draws `(o)`/`( )` rows.
- **`ComboBox`** - a controlled dropdown (`CBS_DROPDOWNLIST`). `int selectedIndex` +
  `list<string> items` (add with `addItem`) + `onChange(index)`. Canvas draws
  `[selected v]`.
- **`ListView`** - a **virtualized** report-mode list. The item source is a callback,
  `Lambda<string(int,int)> rowText` (row, col -> cell text), NOT a copied list, so
  `rowCount` can be 100k+ and only the visible cells are ever queried (Win32
  `LVS_OWNERDATA` + `LVN_GETDISPINFO`). Columns via `addColumn(title, widthDip)`;
  controlled `selectedIndex`; `onSelect(row)` on a selection change, `onActivate(row)`
  on double-click / Enter. Driver helpers: `nativeListSelect(keyPath, row)`,
  `nativeListSelectedRow(keyPath)`, `nativeListCellText(keyPath, row, col)`,
  `nativeListRowCount(keyPath)`.

**`setListOp` seam (v13).** Property setters cannot express a data control's columns +
item source, so the `NativeHost` interface gained ONE op-coded call:
`setListOp(u64 h, int op, int arg0, int arg1, const char* text, u64 payload)`, with the
`LISTOP_*` codes (RESET_COLUMNS / ADD_COLUMN / SET_ROWCOUNT / SET_ROWTEXT_CB /
SET_SELECT_MODE / SET_SELECTION / INVALIDATE). The op set is the deliberate common
denominator of Win32 `ListView`, macOS `NSTableView`, and WinUI `ItemsView` (see the
design block above the method in `ui_native.cb`). The `rowText` callback rides as an
opaque `u64` (a ui.cb `ListRowBox*`, invoked via `__uiListRowText`) so no `Lambda` type
crosses the seam - the same constraint `ctx.post` obeys. The CocoaHost (real `NSTableView`
dataSource) and WinUI3Host (host-side box/model source the `native*` drivers query, its third
seam validation) both implement `setListOp` fully; see the parity matrix.

**Navigation chrome (v14).** Tier-2 controls, each with a Canvas fallback and a headless
self-test in `example/ui/gallery/gallery.cb`; `example/ui/fedit/fedit.cb` (fedit v2) is
the flagship that composes all of them:

- **`TabControl` / `TabPane`** - keyed child panes with **lazy inactive tabs**: only the
  active pane's controls map to native (switching a tab destroys the old pane's controls
  and creates the new pane's), so per-tab editor buffers must live in the app model, not
  an inactive native control. Controlled `selectedTab` + `onSelectTab(index)`. Win32 maps
  the header strip to a `WC_TABCONTROL` (`SysTabControl32`); Canvas draws a bracketed
  header row plus the active pane. Tabs are inserted through the `setListOp` seam
  (`LISTOP_TAB_RESET`/`ADD`/`SET_SEL`).
- **`TreeView`** - an **expand-on-demand** tree whose node source is three callbacks keyed
  by an integer nodeId: `childCount(nid)` / `childId(nid, i)` / `label(nid)`, with
  `TREE_ROOT` (the virtual root) giving the top-level nodes. Children are queried only
  when a node is expanded (Win32 `WC_TREEVIEW` + `TVN_ITEMEXPANDING`), so the tree is
  virtualized. Controlled `selectedNode`; `onSelect(nid)` on a selection change,
  `onExpand(nid)` on expand. The node source rides the `setListOp` seam as an opaque
  `u64` (`TreeBox*`, `LISTOP_TREE_SET_SRC_CB`/`REBUILD`), like `ListView`'s `rowText`.
  Driver helpers: `nativeTreeRootItem`/`nativeTreeChildItem`/`nativeTreeItemCount`/
  `nativeTreeExpandItem`/`nativeTreeSelectItem`/`nativeTreeItemNodeId`.
- **`SplitView`** - two panes split by `int ratio` (the first pane's percentage of the
  split axis) with a 1-DIP divider gutter; `bool vertical` picks side-by-side vs stacked.
  A layout container (not a native control): the layout engine learns the weighted
  two-pane constraint. Dragging the divider fires `onRatioChange(newRatio)` (Win32 hit-
  tests the gutter in the parent `WndProc` with `SetCapture`; `nativeSplitterDrag(keyPath,
  dipX, dipY)` drives it headlessly). Canvas draws the two panes and the divider line.
- **`ContextMenu`** - a per-element right-click menu reusing the P3 declarative menu model
  (`addItem(label, cmd)` / `addSeparator()`). The app builds one (the `<ContextMenu/>`
  sugar works) and registers it by key path with `nativeSetContextMenu(keyPath, (u64)m)`;
  the window takes ownership. On a right-click over that control the Win32 host shows a
  `TrackPopupMenu` and routes the chosen command through the SAME app menu handler
  (`setMenuHandler`), like a menu-bar item. `nativeFireContextMenu(keyPath, itemIndex)`
  routes an item headlessly.
- **Toolbar (pattern, not a new element)** - a toolbar is a styled row of Buttons:
  `View* toolbar()` returns a `DIR_ROW` View with a small gap; add `button(...)`s to it.
  There is no new native control (icon support arrives with `Image` below).

**Visuals + escape hatch (v15).** The last three elements complete the set, each with a Canvas
fallback and a headless self-test in `example/ui/gallery/gallery.cb`:

- **`Image`** - a bitmap leaf. Its content is a toolkit-neutral **32-bit BGRA** buffer, not a
  Win32 `HBITMAP`: the app hands down a BORROWED top-down BGRA32 buffer (`pixels` + `pxW`/`pxH`,
  stride `pxW*4`) that the native host uploads through the new `setImageData` seam (a top-down
  DIB section + `STM_SETIMAGE` on Win32; a `WriteableBitmap`/`NSImage` on the other hosts, P11).
  The app owns the buffer (a component field / global freed on teardown) - the element never
  owns pixel memory. Alternatively `source` names a `.bmp` file the host decodes directly
  (`LoadImage`; WIC/PNG/JPG decode is the same seam with a richer host-internal decoder,
  deferred). Canvas hosts draw a bordered placeholder with the `altText`. Build one with
  `image(pixels, pxW, pxH)` or `<Image .../>` + field assignment.
- **`GroupBox`** - a titled group frame (a `BS_GROUPBOX` BUTTON on Win32; its children position
  as siblings inside the frame). Canvas hosts draw a bordered box with the title on the top edge.
  Build with `groupBox(title, style)` or `<GroupBox title=... />` + `.add(child)`.
- **`CanvasView`** - the **escape hatch**. It hosts a child surface the app paints with the SAME
  `Canvas` API the framework paints through: `onPaint(Canvas)` receives a Canvas whose cell
  origin is the view's top-left, and the app draws text/rects freely (a chart, a game board, any
  custom widget). On a native host the view maps to a GDI-backed child window that invokes the
  boxed closure on `WM_PAINT`/`WM_PRINTCLIENT` (through the `CanvasBox` seam, an opaque `u64`
  like `ListRowBox`); on a Canvas host, `paint()` delegates straight to `onPaint`. Build with
  `canvasView(onPaint)` or `<CanvasView .../>` + `.onPaint = (Canvas c) => {...}`.

**Accent buttons (v15).** A themed push button no longer stays light in dark mode. The Win32 host
draws it via `NM_CUSTOMDRAW` (a documented API, per the look policy): a themed `Button`
(`theme.buttonBg` set) fills with the accent color, shades on hover/press (`CDIS_HOT`/`SELECTED`),
draws a focus ring when focused, and paints its label in `theme.buttonText`. A monochrome
(default) theme stores no accent, so the button keeps the native look (`CDRF_DODEFAULT`) - the
per-control accent is set through the `setAccent(h, fg, bg)` seam from `_syncProps`. Note:
`PrintWindow` re-renders controls in their default style and does NOT route `NM_CUSTOMDRAW`, so
the accent fill is a live-render feature (verified headlessly by the gallery's pixel-level draw
assert), not something the screenshot workflow below captures.

**Widget-gallery screenshots.** `example/ui/gallery/gallery.cb` is the full-element-set flagship
(every element, light + dark, a 25-assert headless self-test). It doubles as the doc-screenshot
source: `gallery.exe --shots <dir>` renders the live window into an offscreen bitmap via
`PrintWindow(PW_RENDERFULLCONTENT)` and writes `gallery_light.bmp` + `gallery_dark.bmp` through
`core/graphic/bitmap.cb` (the same technique `win32_shot.cb` uses for the Canvas host). The
single-column gallery is taller than one screen, so the capture shows the top band (the classic
forms + the `Image`/`CanvasView` visuals, placed high for this reason); the FULL element set is
covered by `--selftest`, not the screenshot.

The tab/tree ops share the ONE `setListOp` seam call rather than adding interface methods
(`LISTOP_TAB_*` = 10-12, `LISTOP_TREE_*` = 20-21). `nativeNodeBounds(keyPath)` returns a
node's laid-out frame (layout assertions). The Cocoa host provides real
`NSSegmentedControl`/`NSOutlineView` backends (P11); the WinUI host provides real
`TabView`/`TreeView` controls with driver-backed strips/nodes (P12). See the parity matrix.

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
StatusBar*   statusBar(string mainText);   // add more panes with .addPart(s)
RadioGroup*  radioGroup(int value, Lambda<void(int)> onChange);  // add options with .addOption(label)
ComboBox*    comboBox(int selectedIndex, Lambda<void(int)> onChange);  // add items with .addItem(s)
ListView*    listView(int rowCount, Lambda<string(int,int)> rowText); // add columns with .addColumn(title, widthDip)
TabControl*  tabControl(int selectedTab, Lambda<void(int)> onSelectTab); // add panes with .addPane(tabPane(title))
TabPane*     tabPane(string title);
TreeView*    treeView(Lambda<int(int)> childCount, Lambda<int(int,int)> childId, Lambda<string(int)> label);
SplitView*   splitView(int ratio, bool vertical);   // add exactly two pane children
View*        toolbar();                              // DIR_ROW View + gap; add button()s
Image*       image(u64 pixels, int pxW, int pxH);    // borrowed top-down BGRA32 buffer
GroupBox*    groupBox(string title, Style style);    // titled frame; add children with .add()
CanvasView*  canvasView(Lambda<void(Canvas)> onPaint);   // app-painted escape hatch
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

    void post(Lambda<void()> work);   // v12: run `work` on the UI thread (thread-safe)
    void assertUiThread();            // v12: debug guard (warns off the UI thread)
};
```

The host owns one `UiContext`, threads `&ctx` into `render()`, and drains
`consumeDirty()` once per loop, re-rendering + reconciling only on a change.
Event closures call `ctx.invalidate()`.

**`ctx.post(work)` (v12) - thread marshaling.** Call from any thread to run `work`
back on the UI thread. The closure is cloned into a heap box and the host marshals it
(Win32 `PostMessage(WM_APP_CALL)`; macOS runs it inline for now - real marshaling is
deferred). With no host bound (TUI/headless), it runs inline. `ctx.assertUiThread()` is
a debug guard that warns if called off the UI thread. Lifetime: the closure is
clone-in / destruct-after-call, so it is leak-clean under `--heap-audit`.

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
stage: instead of painting, it drives real OS controls (HWND on Windows; AppKit
`NSView`/`NSControl` on macOS) so the OS handles rendering, text input, IME, and
accessibility. The app-facing model (Element tree, Component, controlled widgets,
Theme, keyed identity) is unchanged - only the output stage differs. The contract
lives in `core/ui_native.cb`; the implementations are `core/ui_native_win32.cb`
(Win32/GDI) and `core/ui_native_cocoa.cb` (AppKit).

**Platform shim.** An app imports `core/ui_native_host.cb` - a file-scope
`if const (__MACOS__)` shim that pulls in the Cocoa host on macOS and the Win32
host elsewhere. Both backends expose the identical public surface
(`runAppGuiNative`, `buildNativeTree`, the `menu*`/`native*` helpers, the driver
API), so a single app source compiles to real OS controls on both platforms.

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
    void setListOp(u64 h, int op, int arg0, int arg1, const char* text, u64 payload); // v13: LISTOP_* item-data batch
    // (v14 reuses setListOp for the tab + tree ops: LISTOP_TAB_* / LISTOP_TREE_*)
    void setImageData(u64 h, u64 pixels, int w, int h2, int stride);   // v15: push BGRA32 pixels to an Image control
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

`core/ui_native_win32.cb` implements `NativeHost` as a `Window` class (one
per top-level window) and adds the pieces a real desktop app needs. All are
documented-API only (no uxtheme ordinals). An app imports this host and calls
`runAppGuiNative(new App(), title)`.

- **Multi-window (v12):** the host is genuinely multi-window - the app holds an
  owning `list<Window*>` driven by ONE message loop. A `Window` owns its top-level
  HWND, root Element/Component, `UiContext`, live-control bookkeeping, and theme.
  App code reaches the window currently being serviced through **`activeCtx()`** /
  **`activeApp()`** (the free-function WndProc sets it per message), so one pair of
  menu/close closures serves every window. Open another window with
  `openAppWindow(new App(), title)`. These accessors are mirrored on the Cocoa host
  (single-window there in this release; multi-window parity is planned).

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
  prompt); `showSecondaryWindow(title, message)` opens a read-only second top-level
  window; `openAppWindow(new App(), title)` opens a full second app window (v12);
  `nativeCloseWindow()` closes the active window (the loop ends when the last one
  closes). Read a status pane with `nativeStatusText(keyPath)`.
- **Flagship demo:** `example/ui/fedit/fedit.cb` - a small native text editor built
  entirely on the above (open/edit/save, find, dirty-close prompt, light/dark, two
  windows). It imports the `ui_native_host.cb` shim, so the same source builds and runs
  on Windows (Win32) and macOS (Cocoa). The hosts are core, so no `-i` is needed. Build:
  `cflat example/ui/fedit/fedit.cb -o out/fedit` (`.exe` on Windows).

## Host-neutral test drivers (v16, WinUI added v17)

Headless self-tests drive real controls through key-path helpers that every native host
implements under the SAME name, so a self-test contains no toolkit-typed calls (no
`SendMessage`, no `objc_msgSend`, no WinRT vtable calls) and runs against any host. The gallery
self-test lives in the host-neutral `example/ui/gallery/gallery_app.cb` (the `GalleryApp`
Component + the 25-assert self-test); its Win32/Cocoa launcher is `example/ui/gallery/gallery.cb`
(imports the `ui_native_host.cb` shim) and its WinUI 3 launcher is `example/ui/winui/winui_gallery.cb`
(imports `ui_native_winui.cb`). All three build the SAME `gallery_app.cb` - a cflat compilation shares
one global scope across its whole import closure, so a sibling host import supplies the drivers.
Each helper routes through the same element-model handler the OS would fire on real input, then
re-renders; readbacks query native control state (or, for the data controls, the host-side box/
model source the `setListOp` seam feeds).

- **Window / app:** `startHeadlessWindow(new App(), title, w, h)` (hidden window at DIP
  size, tree built), `activeApp()` / `activeCtx()`, `hostDark()`, `hostWindowCount()`,
  `nativeTeardownForTest()` (leak-gate cleanup).
- **Buttons / state:** `nativeClickButton(keyPath)` (Button/Checkbox/RadioButton - mirrors
  the OS auto-toggle then routes), `nativeIsChecked(keyPath)`, `nativeIsEnabled(keyPath)`
  (Win32 `IsWindowEnabled`; Cocoa `NSControl.isEnabled`; WinUI element-model read).
- **Combo:** `nativeComboSelected(keyPath)`, `nativeComboSelect(keyPath, idx)`.
- **Text:** `nativeTypeText(keyPath, s)` (types + routes the change),
  `nativeControlText(keyPath)`, `nativeSetControlText(keyPath, s)`, `nativeStatusText(keyPath)`.
- **Range:** `nativeProgressValue(keyPath)`, `nativeSliderSet(keyPath, v)`.
- **ListView:** `nativeListRowCount`, `nativeListCellText(keyPath, row, col)`,
  `nativeListSelect(keyPath, row)`, `nativeListSelectedRow`, `nativeListActivate(keyPath, row)`.
- **Tabs / tree / split:** `nativeTabSelected`/`nativeTabCount`/`nativeTabSelect`,
  `nativeTreeRootItem`/`nativeTreeChildItem`/`nativeTreeItemCount`/`nativeTreeExpandItem`/
  `nativeTreeSelectItem`/`nativeTreeItemNodeId`, `nativeSplitterDrag(keyPath, dipX, dipY)`,
  `nativeNodeBounds(keyPath)`.
- **Chrome / visuals:** `nativeSetContextMenu(keyPath, menuPtr)`,
  `nativeFireContextMenu(keyPath, itemIndex)`, `nativeImageHasBitmap(keyPath)`.
- **Tooltip / capture:** `nativeTooltipCount()` (Win32: tools registered on the window's shared
  tooltip control; Cocoa/WinUI return 0 - their tooltips ride a per-view / attached property, not a
  per-window registry, and no gate on those hosts exercises the count), `nativeSaveWindowBmp(path)`
  (Win32 offscreen capture via `PrintWindow` + `GetDIBits` -> 24bpp BMP; Cocoa/WinUI no-op returning
  false). The screenshot driver is surfaced host-neutrally as `ui_test.cb`'s `shot()` / `uiShot()`.
- **Accent assert:** `nativeAccentBg(keyPath)` + `nativeTestCustomDrawFill(keyPath)`. On Win32
  these verify the NM_CUSTOMDRAW accent fill (a live-render feature PrintWindow cannot capture) by
  driving the custom-draw path into a memory DC and reading back the filled pixel. On Cocoa they
  return 0 (accents ride `bezelColor`, so there is no stored COLORREF to read). On WinUI 3 there is
  no NM_CUSTOMDRAW; the accent is stored (a brush intent) and BOTH helpers return that stored accent
  color - the fidelity check is "reported fill == stored accent", with no offscreen pixel readback.

## Hosts

- **TUI** (`example/ui/host.cb`, `example/ui/boxes.cb`): a double-buffered
  terminal host. Real console = interactive; redirected I/O (`--run`, CI) falls
  back to a deterministic headless self-test that gates `example.bat` on behavior
  AND leaks (built with `--heap-audit`).
- **Win32** (`example/ui/win32host.cb` + `example/ui/win32_boxes.cb`): a native
  GDI host reusing the framework unchanged behind the `Canvas` seam.
- **Native OS controls** (`core/ui_native_host.cb` shim -> `core/ui_native_win32.cb`
  / `core/ui_native_cocoa.cb`): the `NativeHost` seam instead of `Canvas` - real HWND
  (Win32) or AppKit controls (macOS). See the `NativeHost` section above.
- **WinUI 3 (Windows App SDK)** (`core/ui_native_winui.cb`): the same `NativeHost` seam
  driving real XAML controls rooted in a `Canvas`, brought up unpackaged via
  `MddBootstrapInitialize2` + `Application.Start`. An app runs on it with `runAppWinui(new App())`;
  the gallery launcher is `example/ui/winui/winui_gallery.cb`. Its self-tests run WITHOUT
  `--heap-audit` (the WinAppSDK runtime keeps process-lived singletons and its own heaps).

### Element x host parity matrix (v17)

Y = real native control; the notes call out deliberate differences and gaps. Cocoa is
compile-checked (`--check --platform macos`); its runtime verification on an arm64 box is
deferred (see the plan). "driver-backed" means the control is created but its item DISPLAY is
answered from the host-side `setListOp` box/model the `native*` drivers query - the seam and data
path are validated, while binding a live virtualized item source is a documented enrichment.

| Element      | Win32                     | Cocoa (compile-checked)   | WinUI 3                              |
|--------------|---------------------------|---------------------------|-------------------------------------|
| Button       | Y BUTTON                  | Y NSButton                | Y Button                            |
| Text         | Y STATIC                  | Y NSTextField             | Y TextBlock                         |
| TextInput    | Y EDIT                    | Y NSTextField             | Y TextBox                           |
| TextArea     | Y EDIT multiline          | Y NSTextView              | Y TextBox (uncontrolled-with-sync)  |
| Checkbox     | Y BUTTON checkbox         | Y NSButton                | Y CheckBox                          |
| RadioGroup   | Y BS_AUTORADIOBUTTON      | Y NSButton radio          | Y RadioButton (controlled via model)|
| ProgressBar  | Y msctls_progress32       | Y NSProgressIndicator     | Y ProgressBar (IRangeBase)          |
| Slider       | Y msctls_trackbar32       | Y NSSlider                | Y Slider (IRangeBase)               |
| ComboBox     | Y COMBOBOX                | Y NSPopUpButton           | Y ComboBox (live items + selection) |
| ListView     | Y virtualized OWNERDATA   | Y NSTableView dataSource  | Y ListView, driver-backed items     |
| TabControl   | Y WC_TABCONTROL           | Y NSSegmentedControl      | Y TabView, driver-backed strip      |
| TreeView     | Y WC_TREEVIEW             | Y NSOutlineView           | Y TreeView, driver-backed nodes     |
| SplitView    | layout container          | layout container          | layout container                    |
| StatusBar    | Y msctls_statusbar32      | Y NSTextField strip       | Y TextBlock strip                   |
| GroupBox     | Y BS_GROUPBOX frame       | Y titled NSBox            | header label (look difference)      |
| Image        | Y HBITMAP DIB             | Y NSImageView             | Y WriteableBitmap (BGRA upload)     |
| CanvasView   | Y GDI child + onPaint     | Y CfCanvasView + onPaint  | placeholder (GAP - no Win2D dep)    |
| ContextMenu  | Y TrackPopupMenu          | Y NSMenu popup            | routed via nativeFireContextMenu    |
| tooltip prop | Y tooltips_class32        | Y setToolTip:             | (not wired; deferred)               |
| ctx.post     | Y PostMessage(WM_APP)     | Y performSelectorOnMain   | Y DispatcherQueue.TryEnqueue        |
| multi-window | Y (Window list)           | single-window (deferred)  | single-window (deferred)            |
| accents      | Y NM_CUSTOMDRAW fill      | bezelColor (no readback)  | stored + reported (no readback)     |
| a11y Name     | Y window text / tooltip   | Y NSAccessibility strings | Y XAML automation Name (text)       |

Deliberate WinUI 3 gaps in this release, all documented and non-silent:
- **CanvasView** is a placeholder Border. Win2D would be a NEW dependency, against the
  self-contained-exe pitch; the escape hatch stays Win32/Cocoa. The `onPaint` closure is still
  owned + freed, but never invoked on WinUI.
- **GroupBox** renders as a header label rather than a titled frame (a look difference; the title
  text is what the self-test reads).
- **ListView / TabView / TreeView** create their real XAML control but their item DISPLAY is
  driver-backed (host-side `setListOp` box/model). Binding a live virtualized `ItemsSource` needs a
  WinRT bindable-collection implementation and is deferred; the seam + rowText/TreeBox callbacks are
  fully exercised (the 100k-row virtualization oracle passes: rowText fires only for queried cells).
- **Multi-window** is single-window (mirrors the Cocoa decision); the gallery gate needs one window.
- **tooltip** prop is not wired to `ToolTipService` yet (no self-test covers it on WinUI).

## Accessibility

Native controls give native accessibility for free - a headline anti-Electron property. The
framework does no custom drawing for the standard controls, so each control's OS accessibility
(MSAA/UIA on Windows, `NSAccessibility` on macOS, the XAML automation peer on WinUI) is the real
one, and its **Name** comes from the control's text:

- **Text-bearing controls** (`Button`, `Checkbox`, `RadioButton`, `GroupBox`, `Text`/STATIC,
  `TabControl` tab labels) expose their window text as the accessible Name automatically - no extra
  work.
- **Textless controls** (`ProgressBar`, `Slider`, `ListView`, `TreeView`, `ComboBox`, `Image`,
  `CanvasView`) have no intrinsic Name. When such a control carries a `tooltip`, the Win32 host
  mirrors it into the control's window text (which these classes do not paint), so a screen reader
  announces a Name. Set a `tooltip` on these to name them (e.g. `prog.tooltip = "download progress"`).
- **Edit controls** (`TextInput`, `TextArea`) keep their text as CONTENT, so their Name must come
  from a preceding `Text` label or an assistive `tooltip` (announced as a tip), never from window
  text.

This is a minimal, name-level pass, not a full custom UIA tree; the standing gallery
self-test reads a textless control's Name back via `nativeAccessibleName(keyPath)`. macOS controls
carry native `NSAccessibility` from their string values + `setToolTip:`; WinUI controls carry the
XAML automation Name from their text (`AutomationProperties.Name` from tooltip is deferred with the
tooltip wiring).

## Keyboard and focus

- **Tab order.** Every mapped Win32 control is created `WS_TABSTOP` and parented directly to the
  top-level window (containers `GroupBox`/`SplitView`/`RadioGroup` are LAYOUT containers, not nested
  HWNDs), so the control set is FLAT and the message loop's `IsDialogMessageA` walks Tab / Shift-Tab
  across the whole set - including `ListView`, `TreeView`, `TabControl`, and `ComboBox` (no
  `WS_EX_CONTROLPARENT` is needed precisely because nothing is nested). The framework's own
  `collectFocusables`/`moveFocus` ring (Canvas hosts) mirrors the same order; OS focus is the source
  of truth on a native host and `ctx.focusKey` is a keyed mirror.
- **Default keys.** Enter activates the default push button, Space toggles a focused
  Checkbox/RadioButton, and the arrow keys move within a `RadioGroup` and a `TreeView` - all standard
  common-control behavior, not reimplemented. Dialog Esc/Enter for the message-box helpers
  (`nativeConfirm`/`nativeInfo`) is the OS `MessageBox` convention.
- **Headless guard.** The Win32 hardening self-test walks the Tab ring by handle
  (`nativeFocusFirstH`/`nativeFocusNextH`), asserts it closes over >= 8 distinct controls, and reads
  the focused key back - a real programmatic focus-traversal check, not "verified by hand".

## Event- and prop-naming convention

Handlers follow one principled convention (React-Native-derived), frozen for release:

| Handler | On which nodes | Fires when |
|---------|----------------|------------|
| `onPress` | `Button` | pressed/activated |
| `onChange` | `Checkbox`, `Slider`, `RadioGroup`, `ComboBox`, `TextArea` | the controlled scalar VALUE changed |
| `onChangeText` | `TextInput` | the text value changed (RN's `TextInput.onChangeText` name) |
| `onSelect` / `onActivate` | `ListView`, `TreeView` | selection changed / row double-click-or-Enter |
| `onSelectTab` | `TabControl` | the active tab changed |
| `onExpand` | `TreeView` | a node expanded (children materialized) |
| `onRatioChange` | `SplitView` | the divider was dragged |
| `onPaint` | `CanvasView` | the view needs painting |

Rationale (why `ComboBox` uses `onChange` but `ListView` uses `onSelect`): a control with a SINGLE
controlled scalar uses `onChange` (like `Slider`); a control with TWO distinct interactions
(select vs activate) names them explicitly (`onSelect`/`onActivate`) rather than overloading
`onChange`. `onChangeText` is kept (not renamed to `onChange`) deliberately for RN familiarity.
These are documented as-is for the freeze; no rename was applied (all would be breaking changes for a
purely cosmetic gain).

## Controlled vs uncontrolled

| Widget | Model | Notes |
|--------|-------|-------|
| `TextInput` | CONTROLLED | component owns `value`; `onChangeText` -> update + `invalidate()` -> value flows back |
| `Checkbox` / `Slider` / `RadioGroup` / `ComboBox` | CONTROLLED | component owns the scalar; `on*` pushes the new value up |
| `ListView` / `TreeView` / `TabControl` | CONTROLLED selection | `selectedIndex`/`selectedNode`/`selectedTab` is owned by the component |
| `SplitView` | CONTROLLED ratio | `ratio` owned by the component; `onRatioChange` pushes drags up |
| `TextArea` | UNCONTROLLED-with-sync | native buffer is the source of truth; `value` is a push, `onChange` a dirty notify (documented large-buffer exception) |
| `ProgressBar` | presentational | no input; `value` is display-only |

## Testing your app

`import "ui_test.cb"` gives you a headless, CI-ready test framework for any ui_native app. It
drives real controls through the [host-neutral `native*` drivers](#host-neutral-test-drivers-v16-winui-added-v17)
on an invisible window and asserts on control + model state - deterministic, no display, no OS
input synthesis. The copy-me template is **`example/ui/testing/`**: `todo_app.cb` (the app) +
`todo_test.cb` (its suite). Copy those two files, swap in your Component, and you have a suite.

**Shared-app-module pattern.** Split the app into a host-neutral **app module** (your Component,
imports only `ui_native.cb` + whatever it needs) and a **test target** that imports a host backend,
`ui_test.cb`, and the app module:

```cflat
// myapp.cb  - the app module (no main; host-neutral, like todo_app.cb / gallery_app.cb)
import "ui_native.cb";
class MyApp : Component { /* render() with setKey on everything you automate */ };

// myapp_test.cb  - the test target
import "ui_native_host.cb";   // the default host: Win32 on Windows, Cocoa on macOS
import "ui_test.cb";
import "myapp.cb";
int main(int argc, char** argv)
{
    UiTestSuite s = uiTestSuite("myapp");
    s.test("save enables after edit", (UiTest* t) => {
        t.launch(new MyApp(), 80, 30);              // fresh invisible window per case
        t.type("root/name", "hello");
        t.expectTrue("save enabled", t.isEnabled("root/save"));
        t.click("root/save");
        t.expectText("saved", "root/status", "Saved");
    });
    return s.run(argc, argv);                        // exit code = the CI gate
}
```

The app module is not compilable on its own (it calls host functions like `hostWidth()` that a
sibling backend supplies through the shared import closure), so exclude it from your build sweeps -
the test target is what you compile. This is exactly how `gallery_app.cb` / `gallery.cb` are split.

**Key discipline (the one rule that matters).** Every element you automate needs an explicit
`setKey` (or a sugar `key=`). The test addresses controls by a stable path - `"root/save"`,
`"root/list"`. An unkeyed node falls back to a positional `"#index"` path that breaks the moment
you reorder or conditionally omit a sibling. Key everything you touch; nest keys by parent
(`"root/split/main/editor"`).

**The `UiTest` / `UiTestSuite` API** (a compact map - use LSP for the exact signatures):

| Group | Methods |
|-------|---------|
| Launch | `launch(new App(), wDip, hDip)`, `pump()` (re-render + drain posted work) |
| Actions | `click`, `type`, `setText`, `comboSelect`, `listSelect`, `listActivate`, `tabSelect`, `sliderSet`, `splitterDrag`, `fireContextMenu`, `fireMenu`, `resize`, `focusFirst`/`focusNext` (all key-path addressed) |
| Readers | `controlText`, `isChecked`, `isEnabled`/`isDisabled`, `comboSelected`, `listRowCount`, `listSelectedRow`, `listCellText`, `progressValue`, `statusText`, `bounds`, `accessibleName`, `focusedKey`, `tabCount`/`tabSelected`, `tooltipCount`, `exists(key)`, `hostIsDark()` |
| Asserts | `expectTrue`/`expectFalse`, `expectBool`, `expectInt`, `expectStr`, `expectText(name, key, want)`, `requireTrue(name, cond)` |
| Wait | `waitUntil(pred, timeoutMs)` - see the rule below |
| Suite | `uiTestSuite(name)`, `s.test(name, body)`, `s.run(argc, argv)` |

Asserts record a pass/fail with the key path + expected/actual in the message; they never abort the
process. A failed `requireTrue` marks the case aborted so the rest of its asserts are skipped
(neither passed nor counted) - use it for a precondition the rest of the case depends on. `run`
prints per-case `PASS`/`FAIL` + a suite summary and returns 0 iff every run case passed. It parses
three flags (unknown flags - e.g. your app's own - are ignored, so they coexist):

- `--list` - print case names and exit 0.
- `--filter <substr>` - run only cases whose name contains `<substr>`.
- `--shot-dir <dir>` - on a failing case, auto-capture a screenshot named after the case.

**The `waitUntil` rule.** Everything a driver does is **synchronous** - `click`/`type`/`select`
route the handler and re-render before they return, so you assert immediately. The ONE exception is
work marshaled back to the UI thread with `ctx.post` (typically from a worker thread): that closure
runs only when posted work is drained. `waitUntil(pred, timeoutMs)` polls `pred()` and pumps posted
work between polls, returning true once it holds. Use it ONLY for post/thread work; reaching for it
elsewhere hides a real synchronization bug behind a timeout. The template's "Load sample" case is the
canonical example (a worker thread posts items back; the test waits for the row count). `waitUntil`
is the only method that sleeps, so a suite that uses it must have `sleep()` in scope
(`import "time.cb"`).

**Screenshots.** `t.shot(path)` / `uiShot(path)` writes a 24bpp BMP of the current window (Win32
only; a documented no-op returning false on Cocoa/WinUI). Pass `--shot-dir <dir>` to `run` to grab
one automatically on each failing case. There is no golden-image compare - font/DPI variance makes
byte-compares flaky; probe specific pixels with `customDrawFill`/`accentBg` instead.

**Hardening kits.** For soak coverage - theme storms, reconcile churn, resize storms - call the
library kits (`themeStorm`, `reconcileStress`, `resizeStorm`) documented in the next section. The
template's last case calls `themeStorm` to show the shape.

**CI recipe.** Build the test target with `--heap-audit`, run it headless with stdin redirected from
nul, and gate on BOTH the exit code and a leak grep:

```bash
cflat myapp_test.cb -i . --heap-audit -o out/myapp_test.exe
out\myapp_test.exe <nul            # exit 0 = all cases passed
# fail the build if the run leaked:
findstr /C:"heap-audit: LEAK" run.log
```

This is precisely what `example.bat`'s `--worker-uitest` does for the template.

**Per-host guarantee levels.** The framework is host-neutral, but what a run proves differs by host:

- **Win32** - full: real controls, `--heap-audit` leak gate, offscreen screenshots, and cases may
  relaunch (each `test()` gets a fresh window). Use it as your primary gate.
- **WinUI 3** - runs, but: NO `--heap-audit` (the WinAppSDK runtime keeps process-lived singletons
  and its own heaps), single launch per process (a second `launch()` after teardown within one
  `Application.Start` session is unsupported - keep a WinUI suite to ONE case), and some readers
  answer from the element model rather than a live control (`isEnabled`, tooltip count).
- **Cocoa** - compile-checked only (`--check --platform macos`) until an arm64 box exists; the
  drivers are implemented but not yet runtime-verified.

## Hardening self-tests

The hardening soaks are library kits in `core/ui_test.cb` (the `import "ui_test.cb"` test framework -
`UiTestSuite` runner + `UiTest` facade over the `native*` drivers). They are host-neutral - any
ui_native app can call them - and the gallery self-test dogfoods them. The gallery self-test
(`gallerySelfTest`, host-neutral in `gallery_app.cb`) runs the 25 element asserts plus the two kits
below in one case, so it is 27/27 on Win32 and WinUI 3 (leak-clean under `--heap-audit` and asan-clean
under `--asan` on Win32):

- **`themeStorm(flips, themeBtnKey, modelDark)`** (theme-flip storm) - clicks the theme toggle `flips`
  times and asserts the host titlebar theme (`hostDark()`) stays in lockstep with the app's model
  (`modelDark()`) after every reconcile.
- **`reconcileStress(steps, mutate, expectedKeys)`** (reconcile stress-soak) - calls `mutate()`
  `steps` times; after each step the native control census (one live handle per key) must exactly
  equal the keys `expectedKeys()` reports (a `list<string>` of key paths), and any key present in two
  consecutive steps must keep the SAME handle (identity preserved across reorder). The gallery drives
  it with a fixed-seed LCG over a churn pool
  (`Button`/`Text`/`Checkbox`/`TextInput`/`ProgressBar`/`Slider`/`GroupBox`/`ComboBox`) for 48 steps.
- **`resizeStorm(rootKey, sizes, sanity)`** - drives `nativeResizeClient` through a list of `uiSize(w, h)`
  values and after each asserts layout sanity: `rootKey` keeps positive bounds, the caller's `sanity()`
  holds, and the layout is responsive (root wider at the widest requested size than at the narrowest).
- **`shot(path)` / `uiShot(path)`** - offscreen screenshot verb (Win32 `nativeSaveWindowBmp`; Cocoa/WinUI
  return false). `UiTestSuite.run` auto-captures a per-case shot on failure when `--shot-dir <dir>` is
  passed; the gallery `--shots <dir>` doc-screenshot path calls the same library verb.

The Win32-only `win32HardeningSelfTest` (in `gallery.cb`, gated out of the Cocoa build) is its own
`UiTestSuite` covering focus-traversal, the `resizeStorm`-backed live relayout, and accessible-Name
readback. Because a `UiTest` case body is a non-capturing `function<void(UiTest*)>` while the kits take
capturing `Lambda<>` closures, a case reads app state through a local downcast (e.g.
`GalleryApp* g = activeApp() as GalleryApp;`) and passes `Lambda` predicates that capture it to the kits.

To run the TUI self-tests directly:

```
x64/Release/cflat.exe example/ui/host.cb --run
x64/Release/cflat.exe example/ui/boxes.cb --run
```
