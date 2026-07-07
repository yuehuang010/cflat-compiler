# Plan: Native UI Framework (Electron-competitor, native OS controls)

Status: P0-P6.5 DONE (Win32 + macOS AppKit + WinUI 3 proof; fedit authored in
JSX sugar). P7-P12 rich-control milestones planned 2026-07-06, not started. Created 2026-07-03; consolidated
2026-07-06 (finished phases summarized; this is the single UI framework plan -
the predecessor ui-framework-v5-sugar-widgets.md was lost with the old
internal/plan/ and its shipped result is example/ui/ at API v8, see doc/UI.md).

## Locked decisions (user, 2026-07-03)

1. Windows tier: **Win32 common controls now, WinUI-3-ready host interface**.
   The NativeHost seam is opaque-handle + property-based so a WinUI 3 backend
   can slot in; no Windows App SDK dependency in v1 (self-contained exe stays
   the anti-Electron pitch).
2. Flagship v1 anchor: **a small real tool - text editor "fedit"** (menus,
   file dialogs, accelerators, multiline text, multi-window). Forces the
   framework past forms-demo scope.
3. Look policy: **native controls + styled accents** via documented APIs only
   (NM_CUSTOMDRAW / WM_CTLCOLOR* on Windows, NSAppearance/bezelColor on
   macOS). Native accessibility, IME, and text rendering come for free - a
   headline differentiator vs Electron.

## Architecture (as built)

Keep the app-facing model shipped in example/ui/ui.cb - retained Element tree,
Component, controlled widgets, keyed identity, Theme, `<View/>` sugar - and
swap the output stage: instead of painting through Canvas, the reconciler
drives create/update/destroy of real native controls, our layout engine
computes frames, the OS paints. React Native architecture minus the JS bridge.

```
App render() -> Element tree -> reconcile (existing)
                                     |
              +----------------------+--------------------+
              v                                           v
        Canvas hosts (kept: TUI/GdiCanvas,          NativeHost seam (ui_native.cb)
        headless/testing path)                 Win32Host | CocoaHost | WinUI3Host
```

Key contracts (all proven through three hosts):

- **NativeHost seam** (`example/ui/ui_native.cb`): opaque u64 handles (HWND /
  NSView* / WinUI element), property-style setters (setText, setBoolProp,
  setIntProp, setFrame, setAccent), measureText host callback, PROP_*/FONT_*
  consts. Nothing toolkit-typed leaks above the seam - that is the whole
  "WinUI-ready" contract.
- **Control lifecycle**: tree-walk-diff keyed by key path against
  `UiContext.nativeByKey` (deviation from the original Patch-stream-consumption
  idea - equivalent and more robust; Patch.key/reconcilePath identity still
  exists in ui.cb). Batched apply (DeferWindowPos on Win32).
- **Layout in DIPs**: layout ints are DIPs; TUI reads 1 DIP == 1 cell (byte-
  identical); native hosts scale DIP->px at the boundary (GetDpiForWindow;
  Cocoa points are already density-independent). measureText replaces the
  1-cell-per-char assumption. Element.nodeBounds() exposes frames.
- **Events and focus**: native event -> Event -> existing dispatch() seam;
  handle->key reverse map routes to elements. The OS owns real focus; our
  keyed focus is a mirror (BN/EN_SETFOCUS). `applying` guard swallows
  self-inflicted EN_CHANGE (the classic native-React reentrancy bug -
  designed in from day one, per plan).
- **TextArea is deliberately UNCONTROLLED-with-sync**: native buffer is the
  source of truth; `value` is a push, `onChange` a dirty notify; propsEqual
  ignores value so re-render never clobbers the buffer. Documented exception
  to the controlled-widget rule for large-buffer widgets.
- **App shell**: declarative menu bar (menuReset/menuAddTop/menuAddItem +
  setMenuHandler) realized as HMENU + accelerator table (parseAccel
  "Ctrl+Shift+S" -> ACCEL) / NSMenu with Ctrl->Cmd mapping; file dialogs via
  comdlg32 GetOpen/SaveFileName (deviation: not IFileDialog COM - visually
  identical, more robust; COM version deferred); nativeConfirm/nativeInfo;
  setCloseQuery WM_CLOSE hook; a secondary top-level window;
  nativeTeardownForTest for leak-gated self-tests.
- **Theming**: per-monitor-v2 DPI + WM_DPICHANGED relayout; Segoe UI Variable
  sized to dpi; DWMWA_USE_IMMERSIVE_DARK_MODE titlebar; WM_CTLCOLORSTATIC/
  EDIT/BTN + WM_ERASEBKGND driven by the app Theme. NM_CUSTOMDRAW accent
  fills DEFERRED (buttons stay OS-look, light in dark mode) -> P10.
- **macOS**: dlopen/objc_msgSend bridge with per-signature fn-ptr casts
  (example/macos/cocoa.cb) - no SDK, no Xcode, no -framework. The arm64
  variadic-ABI spike ran first and passed with NO compiler prerequisite
  (incl. NSRect 4-double HFA return). CocoaHost uses a FLIPPED content view
  (top-left coords like Win32), fixed BASE_X=8/BASE_Y=26, no DPI machinery.
- **WinUI 3**: `winrtDelegate(DelegateType, closure)` compiler builtin
  (P6 M1) synthesizes a refcounted COM-callable object around a cflat
  closure, metadata-driven for ANY winmd delegate (derived PIID for
  parameterized ones); closure cloned in, destructed on final Release.
  Unpackaged XAML app boot needs NO IApplicationOverrides aggregation:
  MddBootstrapInitialize2 -> RoInitialize STA -> Application.Start(cb) ->
  in-callback base Application via IApplicationFactory::CreateInstance
  (null outer) + Window + controls (winui_app_demo.cb). WinUI3Host
  (winui_host.cb) implements the same NativeHost contract: controls rooted
  in a Canvas, setFrame -> Canvas.SetLeft/SetTop, events via winrtDelegate,
  UIA-driven headless self-tests.

## Completed phases (summary; details in git history + doc/UI.md v10)

All gates green at each step: test.bat Release, example.bat Release
(self-tests under --heap-audit unless noted), test_lsp when compiler touched.

- **P0 (2026-07-03) - seams, no visible change.** Patch.key identity +
  reconcilePath, UiContext.nativeByKey, DIP reframing with TUI cell adapter,
  NativeHost interface, doc/UI.md API v9. Canvas/TUI self-tests
  byte-identical.
- **P1 (2026-07-03) - Win32Host MVP.** win32_native_host.cb: real BUTTON/
  STATIC/EDIT/checkbox/msctls_progress32/trackbar/etched-panel; batching;
  WM_COMMAND/WM_HSCROLL routing; OS Tab via IsDialogMessageA; focus mirror;
  EN_CHANGE guard. win32_native_settings.cb headless self-test 7/7
  (--worker-native in example.bat).
- **P2 (2026-07-03) - modern look.** DPI v2, WM_DPICHANGED, dark titlebar,
  WM_CTLCOLOR theming, dpi-sized Segoe UI Variable. Verified at 200% dpi.
- **P3 (2026-07-03) - editor essentials.** TextArea element + host; menus/
  accelerators; comdlg32 dialogs; MessageBox helpers; close-query; secondary
  window. ListView + ctx.post deferred -> P8/P7.
- **P4 (2026-07-03) - flagship fedit.** example/ui/fedit/fedit.cb: open/save/
  save-as, Ctrl+F/F3 find, dirty marker + close prompt, Ctrl+T light/dark,
  About window. Headless self-test 6/6 (--worker-fedit), leak-clean,
  PrintWindow-verified visually.
- **P5 (2026-07-05) - macOS AppKit host** (commits ad01bf8/50b4807/e5058ef).
  objc spike first (passed, no compiler work), cocoa_native_host.cb to P1-P3
  parity (NSButton/NSTextField/NSSlider/NSProgressIndicator/NSBox/NSTextView,
  NSMenu), native_host.cb `if const (__MACOS__)` import shim - one app source
  compiles against either backend; fedit runs unchanged. Gate: example_mac.sh
  + test.sh/test_lsp.sh on the arm64 box (HeapAudit is Windows-only; leak
  gate = shared nativeTeardownForTest path).
- **P6 M1-M3 (2026-07-03) - WinUI 3 track.** M1: winrtDelegate builtin
  (grammar + WinmdExtract + LLVMBackend + MainListener; test_winmd 10/10
  incl. a REAL async Completed handler called by Windows). M2: unpackaged
  XAML window headless (winui_app_demo.cb, UIA-invoke self-test, exit 0).
  M3: WinUI3Host behind the unchanged NativeHost seam (winui_demo.cb runs a
  real Component through the same ui.cb reconcile). Two compiler changes:
  winmd value-struct registration is first-writer-wins (user's `struct Rect`
  no longer clobbered), and closure captures of owning `string` are
  deep-copied into the heap env with per-closure cleanup (string only).
  winui self-tests gated WITHOUT --heap-audit (WinAppSDK process-lived
  singletons + own heaps; documented in example.bat). M3 deferred: TextBox,
  Slider/ProgressBar, theme accents, multi-window -> P11.

Compiler fallout fixed along the way (both 2026-07-03, regression-tested):
interface-bind of an owning-value class is now a BORROW (was move+zero of the
source), and ternary over owning strings is clone-by-default deep-copy (was
alias+free heap corruption). Deferred issues filed at the time:
multiple-import-dirs-last-wins, ternary-owned-return-without-move-leaks.

## Next milestones (P7+): rich control set (planned 2026-07-06)

Goal: move from "forms demo + editor" to a complete framework. Current element
inventory (ui.cb): View, Text, Button, Box, TextInput, Checkbox, Progress,
Slider, TextArea, Scroll, Component. Missing for "rich": data controls
(list/tree/combo/tabs/radio), app-shell chrome (toolbar/statusbar/context
menu/splitter/tooltip), real multi-window, thread marshaling, image, and
host parity. Sequencing principle stays the same as P0-P4: element + Win32Host
first (it is the reference host with the strongest test harness), then a
parity milestone for Cocoa, then WinUI; every new element ships with a
headless state-assert self-test under --heap-audit and a doc/UI.md entry.

Cross-cutting rules for all of P7-P12:
- New elements are added behind the SAME NativeHost property surface
  (createControl elemKind + setText/setBoolProp/setIntProp + new
  setListOp/setImageData batch calls where noted) - nothing Win32-typed may
  leak above the seam (the WinUI-ready contract).
- Canvas hosts get a minimal fallback rendering for each new element (list ->
  text rows, tabs -> text row, etc.) so TUI/GdiCanvas self-tests keep guarding
  layout+reconcile for the whole element set.
- Each milestone gates on: test.bat Release green, example.bat Release green
  (new self-tests included), test_lsp green if any compiler work happens.
- **Sugar-first authoring**: ui.cb's job is to abstract the native platform
  well enough that the library-agnostic <Tag/> element sugar is the natural
  authoring syntax. Every new element keeps its props as plain public fields
  on a default-constructible class (factories stay thin field-setters), so
  sugar coverage is automatic; each new element's self-test authors at least
  one instance via sugar.

### P6.5 - declarative sugar demonstration: fedit authored in JSX

DONE 2026-07-06. Editor.render() converted to sugar exactly as specified below
(root <Box>/<Text> tree, conditional find-bar and TextArea via root.add(<.../>),
identical keys/props/closures). NO compiler or ui.cb change was needed - the
sugar handled string-literal attrs, {expr} attrs (incl. lambdas and computed
strings), and Box* assignment as-is. Gates: fedit self-test 6/6 leak-clean
under --heap-audit, example.bat Release 84/0/25, counter_jsx equivalence PASS.

Goal: prove the <Tag/> sugar composes over the CURRENT control set through a
real app, not just counter_jsx.cb's 3-element identity test. The sugar is
library-agnostic (tag = type in scope, attribute = field init, child = add())
and every ui.cb element already exposes sugar-ready fields - no compiler and
no ui.cb change is expected; this is an authoring-style conversion.

- Rewrite Editor.render() in example/ui/fedit/fedit.cb using element sugar:
  <Box key="root" style={...}> with <Text key="status" text={...}/>, the
  find-bar <TextInput key="find" value={...} onChangeText={...} width={40}
  placeholder="..."/>, and <TextArea key="editor" value="" onChange={...}
  width={...} height={...}/>. The find bar stays a conditional root.add()
  (sugar children are static by design) - deliberately demonstrating that
  sugar and imperative composition mix.
- Behavior-identical: same keys (native EDIT reuse across find-bar toggles
  depends on the stable "editor" key), same uncontrolled TextArea contract,
  same closures.
- Gate: fedit headless self-test 6/6 + example.bat Release green on Win32
  AND the macOS shim path unaffected (no host code touched); leak-clean
  under --heap-audit (owned-string attributes beside sibling capturing
  closures is the exact shape counter_jsx.cb's flush note covers).

### P7 - app-shell foundation (unblocks everything after)

The two deferred P3 items plus real multi-window. Do this first because data
controls and a bigger flagship both sit on it.

- **ctx.post() thread marshaling**: host postToUiThread wrapping
  function<void()> (PostMessage(WM_APP_CALL) on Win32;
  performSelectorOnMainThread on Cocoa). ctx.assertUiThread() debug guard.
  Closure lifetime audit under --heap-audit (clone-in, destruct-after-call;
  the winrtDelegate work already proved the pattern).
- **Window class / multi-window generalization**: kill the single-editor
  host globals. Window owns {native top-level, root Element, UiContext,
  menu bar, close-query}; app = list<Window>, one message loop. fedit gains
  "New Window" = a full second editor (the P4 deviation). Secondary/About
  window path refactors onto it.
- **StatusBar element** (msctls_statusbar32 / custom NSView strip): parts +
  text per part. fedit status line moves onto it.
- **Tooltip support** (tooltips_class32 / NSView toolTip): a `tooltip` string
  prop on every element, not a new element kind.
- Gate: fedit self-test extended (two live editor windows, post() round-trip
  from a worker thread, statusbar readback).

### P8 - data controls, tier 1 (the "rich" core)

- **RadioGroup/RadioButton** (BS_AUTORADIOBUTTON + WS_GROUP / NSButton
  radio): controlled, single `value` on the group.
- **ComboBox/Dropdown** (COMBOBOX CBS_DROPDOWNLIST / NSPopUpButton):
  controlled selectedIndex + items list<string>; onChange.
- **ListView** (WC_LISTVIEW report mode / NSTableView): columns, rows,
  selection (single+multi), onSelect/onActivate (double-click/Enter).
  Virtualized from day one via LVS_OWNERDATA + a rowText(row,col) callback -
  the item source is a callback, not a copied list, so 100k rows is v1
  behavior, not a retrofit. NSTableView is naturally virtualized via its
  dataSource, so the same element contract maps cleanly.
- **NEW seam call**: setListOp(h, op, ...) batch protocol for
  columns/rowCount/invalidate - property sets alone cannot express item data.
  Design it against BOTH ListView and NSTableView before coding (and sanity-
  check against WinUI ItemsRepeater so P11 does not force a redesign).
- Gate: new example widget-gallery app (see P10) starts here as the
  self-test carrier for these controls; fedit is NOT forced to use them yet.

### P9 - data controls, tier 2 + navigation chrome

- **TabControl** (WC_TABCONTROL / NSTabView... use tabless NSTabView or
  segmented control + swap, decide in-milestone): keyed child panes, lazy
  render of inactive tabs (only active pane reconciles to native).
- **TreeView** (WC_TREEVIEW / NSOutlineView): virtualized-ish via expand-on-
  demand callback (children(nodeId)); onExpand/onSelect.
- **ContextMenu**: reuse the declarative menu model from P3 as a per-element
  right-click menu (TrackPopupMenu / NSMenu popUpMenuPositioningItem).
- **Toolbar**: v1 = a styled View row of Buttons (no new native control;
  document as pattern), icon support arrives with Image in P10.
- **Splitter/SplitView element**: two panes + draggable divider. Win32 =
  custom-hit-test child; Cocoa = NSSplitView. Layout engine learns one new
  constraint (weighted two-pane).
- Gate: fedit v2 - file-tree sidebar (TreeView) + multi-tab documents
  (TabControl) + splitter + context menu on the tree + toolbar. This is the
  flagship forcing-function for the whole tier, same role P4 played.

### P10 - visuals + the widget gallery flagship

- **Image element**: setImageData(h, ptr, w, h, stride) BGRA32 at the seam
  (STM_SETIMAGE HBITMAP / NSImageView). Decoder = WIC on Windows,
  CGImageSource on macOS, behind a tiny host call (no image lib dependency).
  Unlocks toolbar icons and About-box logos.
- **GroupBox** (BS_GROUPBOX / NSBox titled) - trivial, closes the classic-
  forms set.
- **NM_CUSTOMDRAW accent buttons** (deferred from P2): themed accent fill +
  focus ring so buttons stop being light in dark mode - the last big
  dark-mode gap. Documented APIs only, per look policy.
- **Custom Canvas widget inside a native host view** (the long-planned
  escape hatch): a CanvasView element hosting a GdiCanvas/CG-backed child
  the app paints via the existing Canvas API. Closes the "framework cannot
  do X" objection permanently.
- **Flagship: example/ui/gallery/gallery.cb** - a widget-gallery app (every
  element, light+dark, per-page headless self-test). Becomes the permanent
  regression carrier + the marketing demo + the doc screenshots source
  (PrintWindow -> BMP per page).
- Gate: gallery self-tests green for the FULL element set on Win32.

### P11 - host parity sweep (macOS, then WinUI)

- **CocoaHost parity** for everything P7-P10 added (NSTableView,
  NSOutlineView, NSTabView, NSPopUpButton, NSSplitView, NSImageView,
  status strip, popup menus, postToUiThread). Port gallery + fedit v2;
  example_mac.sh extended. Menu-bar-first mac conventions audited once
  (Cmd shortcuts, app menu).
- **WinUI3Host catch-up** (the M3 deferred list + the new set): TextBox,
  Slider/ProgressBar (IRangeBase), theme accents, multi-window, then
  ListView->ItemsView/ListView, ComboBox, TabView, TreeView. This is where
  the setListOp seam design is validated a third time.
- Gate: gallery runs on all three hosts; parity matrix table added to
  doc/UI.md (element x host, with deliberate gaps documented, not silent).

### P12 - promotion to core/ (the "complete framework" stamp)

- Name the framework (bikeshed lands here), move ui.cb + ui_native.cb +
  hosts into core/ with DeploymentContent entries; `import "ui.cb"` just
  works with no -i gymnastics (also kills the multiple-import-dirs pain for
  UI apps).
- API freeze pass over doc/UI.md: one editing sweep for consistency
  (prop naming, event naming, controlled vs uncontrolled table), version it
  v1.0.
- test.bat picks up a Test/test_ui.cb smoke (headless, no window shown - or
  keep it in example.bat if window creation is too environment-sensitive;
  decide at the milestone).
- Stretch (explicitly optional, do not block v1.0): DatePicker, hyperlink
  (SysLink), font/color pickers, drag-and-drop, clipboard helpers,
  accessibility pass (UIA names via SetWindowText/accName).

### Sequencing rationale

P7 before P8 because ListView/TreeView demos and fedit v2 need multi-window
+ post(); P8 before P9 because the setListOp seam (hardest new interface
design) should be proven on ListView before TreeView/Tab reuse it; gallery
lands mid-stream (P10) so parity (P11) has a single carrier to port; core/
promotion (P12) is last so the API freeze happens after three hosts have
stress-tested the seam. Compiler work expected: NONE planned - every control
above rides existing interop (proven by P1-P6); if one surfaces (likely
candidates: callback-heavy LVS_OWNERDATA marshaling, WIC COM edge), file it
in internal/issue/ and escalate per the P6 M1 precedent.

## Testing strategy

- example.bat stays the gate, all UI self-tests under --heap-audit (except
  the winui ones - WinAppSDK heaps, documented in example.bat).
- Canvas hosts keep the pixel-level asserts (guiSelfTest et al) - they
  primarily guard layout + reconcile, which are shared with native hosts.
- Native host tests are STATE asserts on invisible windows (Win32) /
  non-activated NSApplication (macOS): create, patch, SendMessage/action,
  read back control state. Interactive-input probes reuse the
  WriteConsoleInput-style injection philosophy (never "verified by hand").
- win32_shot.cb-style screenshots: native variant via PrintWindow into a DIB
  for occasional visual review (used to verify fedit).

## Risks / open items

Resolved during P0-P6: objc_msgSend signature casts (spike passed, no
compiler work); EN_CHANGE reentrancy (guard designed in from P1); flicker
(DeferWindowPos sufficed); Patch/identity refactor (P0 gated byte-identical).

Still open going into P7+:
1. Win32 dark mode for control interiors is officially unsupported - covered
   via WM_CTLCOLOR + custom-draw accents (P10); stock scrollbars stay light.
2. Menu/dialog/post() closures held by native structures - lifetime audit
   under --heap-audit at every milestone (closures are owning values;
   teardown must destruct).
3. setListOp seam design (P8) must survive three hosts - design against
   ListView + NSTableView + WinUI ItemsView before coding.
4. Multi-window refactor (P7) touches the host globals every existing
   self-test relies on - migrate tests in the same change, gate on all of
   them.

## Naming

Working name "fedit" for the demo app; framework itself stays `ui.cb` until
core/ promotion (name bikeshed lands in P12).
