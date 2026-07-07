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

- **Framework API version:** 15 (v5 added color to `Style` + `Canvas`; v6 added the
  `Theme` styling preference and hover/pressed/focus interaction states; v7 added the
  `Checkbox` widget, the `disabled` state, and `Style.gap`; v8 added the `ProgressBar`
  and `Slider` widgets with the `track`/`accent` theme slots; v9 added the `NativeHost`
  seam - real OS controls as an alternative output stage to `Canvas` - plus `Patch.key`
  identity, the `UiContext.nativeByKey` shadow map, and the DIP layout unit; v10 added
  the `TextArea` multiline element and the Win32 native host's editor features -
  per-monitor DPI, dark titlebar + themed controls, a declarative menu bar with
  accelerators, shell file dialogs, message boxes, and a secondary window; v11 added
  the macOS **AppKit (Cocoa)** `NativeHost` backend and the `native_host.cb` platform
  shim, so one app source compiles to real OS controls on both Windows and macOS;
  v12 added the app-shell foundation - real multi-window (a `Window` class + one
  message loop, `activeCtx()`/`activeApp()`/`openAppWindow`), `ctx.post()` thread
  marshaling, the `StatusBar` element, and a `tooltip` prop on every mappable element;
  v13 added the tier-1 data controls - `RadioGroup`/`RadioButton`, `ComboBox`, and a
  virtualized `ListView` - plus the `setListOp` item-data seam call; v14 added the tier-2
  navigation chrome - a `TabControl`/`TabPane` (keyed panes, lazy inactive tabs), a
  `TreeView` (expand-on-demand node source), a `SplitView` (two weighted panes + a
  draggable divider), a per-element `ContextMenu`, and a `toolbar()` pattern - reusing the
  `setListOp` seam for the tab/tree ops; v15 completed the element set - an `Image` (BGRA32
  pixels pushed through the new `setImageData` seam), a titled `GroupBox`, and a `CanvasView`
  escape hatch the app paints with the `Canvas` API - plus NM_CUSTOMDRAW accent buttons and the
  widget-gallery screenshot workflow)
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
crosses the seam - the same constraint `ctx.post` obeys. The CocoaHost + WinUI3Host
implement `setListOp` as stubs today (freeing the box); the real table backends land in
the P11 host-parity sweep.

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
node's laid-out frame (layout assertions). The Cocoa + WinUI hosts get placeholder
controls + driver stubs for these; real `NSTabView`/`NSOutlineView`/`NSSplitView` /
`TabView`/`TreeView` backends land in the P11 host-parity sweep.

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
lives in `example/ui/ui_native.cb`; the implementations are `win32_native_host.cb`
(Win32/GDI) and `cocoa_native_host.cb` (AppKit).

**Platform shim.** An app imports `example/ui/native_host.cb` - a file-scope
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

`example/ui/win32_native_host.cb` implements `NativeHost` as a `Window` class (one
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
  windows). It imports the `native_host.cb` shim, so the same source builds and runs
  on Windows (Win32) and macOS (Cocoa). Build:
  `cflat example/ui/fedit/fedit.cb -i example/ui -o out/fedit` (`.exe` on Windows).

## Hosts

- **TUI** (`example/ui/host.cb`, `example/ui/boxes.cb`): a double-buffered
  terminal host. Real console = interactive; redirected I/O (`--run`, CI) falls
  back to a deterministic headless self-test that gates `example.bat` on behavior
  AND leaks (built with `--heap-audit`).
- **Win32** (`example/ui/win32host.cb` + `example/ui/win32_boxes.cb`): a native
  GDI host reusing the framework unchanged behind the `Canvas` seam.
- **Native OS controls** (`example/ui/native_host.cb` shim -> `win32_native_host.cb`
  / `cocoa_native_host.cb`): the `NativeHost` seam instead of `Canvas` - real HWND
  (Win32) or AppKit controls (macOS). See the `NativeHost` section above.

To run the TUI self-tests directly:

```
x64/Release/cflat.exe example/ui/host.cb --run
x64/Release/cflat.exe example/ui/boxes.cb --run
```
