# Plan: Native UI Framework (Electron-competitor, native OS controls)

Status: P0-P14 DONE - framework PROMOTED TO core/ as `ui_native` (2026-07-07).
(Win32 + macOS AppKit + WinUI 3; fedit authored in
JSX sugar; app-shell foundation - multi-window, ctx.post, StatusBar, tooltip; P8
data controls tier 1 - RadioGroup/ComboBox/virtualized ListView + the setListOp seam;
P9 tier-2 navigation chrome - TabControl/TreeView/SplitView/ContextMenu/toolbar + fedit v2;
P10 visuals + the widget-gallery flagship - Image/GroupBox/CanvasView, NM_CUSTOMDRAW accent
buttons, and the full-element-set gallery with light/dark PrintWindow screenshots;
P11 macOS parity sweep - gallery ported onto the host-neutral driver surface + verified on
Windows, and real Cocoa backends for the P7-P10 stubs, compile-checked via
`--check --platform macos` with mac RUNTIME verification DEFERRED;
P12 WinUI 3 parity sweep - the full-element WinUI3Host + the gallery split into a
host-neutral gallery_app.cb, so the 25-assert gallery self-test runs GREEN on WinUI 3
- the setListOp seam's third host validation, retiring the last pre-freeze design risk);
P13 (hardening + API-freeze prep) - the compiler-workaround removal sweep, keyboard/focus
+ minimal a11y audit, relayout/theming hardening, a seeded reconcile stress-soak, and the
doc/UI.md v1.0-rc freeze. P14 core/ promotion (as `ui_native`) is DONE. Replanned 2026-07-07
(the old single P11 parity sweep split in two, promotion pushed from P12 to P14).
CAVEAT (carried from P11, still open going into P14): macOS is COMPILE-CHECKED ONLY
(`--check --platform macos`); no arm64 box was available, so mac RUNTIME verification of
the gallery/fedit self-tests is DEFERRED and MUST be done before P14 signs off on mac.
Created 2026-07-03; consolidated
2026-07-06 (finished phases summarized; this is the single UI framework plan -
the predecessor ui-framework-v5-sugar-widgets.md was lost with the old
internal/plan/ and its shipped result is example/ui/ at API v8, see doc/UI.md).

## Post-merge macOS re-port status (2026-07-07) - DONE ON MAC; Windows re-verify owed

Context: merged origin/master `a8ef6c4` (fast-forward, 3 commits: closure-generics +
delete-of-alias compiler work; the UI core migration `9952c74`; the `ui_test.cb` test
framework `a8ef6c4`). The migration MOVED `example/ui/cocoa_native_host.cb` ->
`cflat/core/ui_native_cocoa.cb` but was branched from BEFORE the verified macOS runtime
fixes, so the merge DROPPED them. fedit is already migrated onto `ui_test.cb` upstream
(2 UiTestSuite cases; the Win32-only nav suite is gated off mac via
`if const (__MACOS__){}else{}`, and `GetCurrentThreadId`/`_postWorker` DCE out) - so
"integrate ui_test into fedit" is already done in the merge; the remaining work is
re-landing the dropped Cocoa host fixes so the mac gate goes green again.

Post-merge baseline (Release compiler rebuilt on the arm64 box): `./example_mac.sh` =
30 passed / 1 failed. Only `fedit` fails (rc=139 SIGSEGV) - it is the sole self-test
that builds a TreeView, hitting the reverted `NSTableColumn initWithIdentifier:`
missing-arg bug. Crash report confirms it: EXC_BAD_ACCESS in `objc_retain` <-
`-[NSTableColumn initWithIdentifier:]` <- `_msg0_...` (the selector was sent via msg0
with no identifier arg). cocoa_probe/cocoa_window/cocoa_settings still PASS (no tree).

Re-port progress (the verified patch is saved at scratchpad/local_review_fixes.patch;
target is `cflat/core/ui_native_cocoa.cb`, re-anchored by hand since the file diverged):

LANDED, part 1 (the first port agent, before it hit a session limit):
- NSTableColumn `initWithIdentifier:` msg0 -> msg1 + nsStr("col") (the fedit crash fix).
- `accHandles`/`accBgs` fields on CocoaHost.
- setText: NSButton + NSBox title path; NSControl-only guard on stringValue/setStringValue:.
- setEnabled: NSTextView vs NSOutlineView split inside scroll; NSControl-only guard.
- `this._storeAccent(h, 0)` call in the control-destroy path.

LANDED, part 2 (main session, same day - completes the port):
- `_storeAccent`/`_accentFor` method bodies + the `setAccent` store call.
- `nativeControlText`/`nativeSetControlText`: NSBox (title) + NSControl (stringValue) guards.
- `nativeTypeText`: NSControl-only guard.
- `nativeAccentBg` -> `_nHost._accentFor(h)`; `nativeTestCustomDrawFill` delegates to it
  (stored-accent readback, WinUI P12 precedent); stale comment fixed.
- `_onTreeWillExpandImp` + `outlineViewItemWillExpand:` registration (the outline's
  setDelegate: was already wired in createControl).
- `nativeListSelect`: routes onSelect after a programmatic selection.
- `hostDrainPosted`: two-pass non-blocking `runMode:beforeDate:` drain.
- `nativeTeardownForTest`: move-out accHandles/accBgs; accessor-comment honesty fix.
- doc/UI.md ctx.post paragraph (performSelectorOnMainThread + hostDrainPosted).
- example_mac.sh: `gallery` selftest_case added (gate is now 32 cases).

NOT re-applied on purpose: the stash's fedit.cb edits (os.thread_current_id, case-13
gating) - the merged fedit already gates the whole nav suite off mac and DCEs
GetCurrentThreadId, so touching fedit.cb was unnecessary (confirmed: fedit PASS unmodified).

VERIFIED (arm64 box, Release):
- `./example_mac.sh` = 32 passed / 0 failed (`fedit PASS`, `gallery PASS`).
- `out/fedit_mac --selftest` 9/9 (SUITE fedit 1/1, rc=0);
  `out/gallery_mac --selftest` 27/27 (SUITE gallery 1/1, rc=0).
- `--check --platform macos` clean: cflat/core/ui_native_cocoa.cb, fedit.cb, gallery.cb.
- `test.sh Release`: 151 passed / 0 failed / 18 skipped.
- NOTE: the compiler reads core libs deployed next to the exe (`x64/Release/core/`); the
  edited `cflat/core/ui_native_cocoa.cb` was hand-copied there. A CMake rebuild redeploys it.

STILL OWED: Windows re-verification (test.bat + example.bat Release). The edits are
mac-only code paths (ui_native_cocoa.cb) plus doc/UI.md and example_mac.sh (inert on
Windows), so risk is low, but the core/ file rides every Windows deploy and must be
gate-checked there before commit.

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

Cross-cutting rules for all of P7-P14:
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

DONE 2026-07-06. Win32 host is now genuinely multi-window: the single-editor host
globals (_nHost/_nApp/_nTree/_nCtx/_nW/_nH) are gone, replaced by a `Window` class
(implements NativeHost, owns hwnd/font/tree/component/UiContext/live-control lists) held
in an owning `list<Window*>`; the free-function WndProc resolves the servicing window by
scanning for its HWND and app code reaches it via activeCtx()/activeApp() (mirrored on
Cocoa). fedit's "New Window" opens a full second editor; the About/secondary window rides
the shared loop. ctx.post(work) clones a closure into a heap PostBox and marshals it to
the UI thread via PostMessage(WM_APP_CALL) (worker-thread round-trip proven, leak-clean);
ctx.assertUiThread() guards. New StatusBar element (msctls_statusbar32 / NSTextField strip
/ Canvas text row) carries fedit's status line; `tooltip` is a plain field on the mappable
elements (tooltips_class32 / setToolTip:). Gate: fedit self-test 10/10 (adds statusbar
readback, StatusBar sugar, two live windows, worker post round-trip) leak-clean +
asan-clean; win32_native_settings 8/8 (adds a tooltip assertion) leak-clean; example.bat
Release 84/0/25; test.bat Release green.

Deviations / notes:
- ctx.post is wired as a UiContext hook (opaque boxed-closure pointer + a `function<>`
  marshal callback the host installs), NOT a NativeHost interface method - a Lambda-typed
  interface/hook parameter breaks the monomorphizer, so the closure crosses the seam as a
  u64. No WinUI3Host churn.
- macOS multi-window + real post marshaling stay single-window / inline stubs (already
  P11 in this plan); the Cocoa host gained the host-neutral accessors + StatusBar/tooltip
  mapping + nativeStatusText so shared fedit compiles and its self-test passes there
  (cflat --check --platform macos green; runtime unverified on this Windows box).
- NO compiler changes. Found+filed a real compiler bug: `expr as T` on a *function-call
  result* yields an OWNING pointer that double-frees at scope exit (a named-local downcast
  is a correct borrow). Worked around with the two-step `T x = call(); U* p = x as U;` in
  fedit + hosts. See internal/issue/downcast-of-call-result-owns-and-double-frees.md. Also
  fixed a latent host bug: `(string)buf`/`(string)path` aliased a freed/stack buffer on
  return (now `"" + buf` for an owned copy) - was UAF, surfaced by asan.

Original spec (for reference):
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

DONE 2026-07-07. Three tier-1 data controls landed behind the unchanged NativeHost
property surface plus ONE new interface method. RadioGroup/RadioButton
(BS_AUTORADIOBUTTON, WS_GROUP on the first radio; controlled - each radio's `checked`
flows down, a click routes one key-path segment up to the group's onChange), ComboBox
(CBS_DROPDOWNLIST, controlled selectedIndex + imperative items, CBN_SELCHANGE), and a
VIRTUALIZED ListView (LVS_OWNERDATA + LVM_SETITEMCOUNT; the rowText(row,col) callback
is the item source, so 100k rows only ever query the visible cells via LVN_GETDISPINFO;
single-select controlled, onSelect on LVN_ITEMCHANGED, onActivate on NM_DBLCLK /
LVN_KEYDOWN Enter). New seam call `setListOp(h, op, arg0, arg1, text, payload)` carries
columns/rowCount/selection/invalidate + the boxed rowText callback (a ui.cb ListRowBox*
as an opaque u64 - no Lambda crosses the seam, mirroring ctx.post); designed against
Win32 ListView + NSTableView + WinUI ItemsView up front (design block above the interface
method in ui_native.cb). Canvas fallbacks render for all three (radio rows, "[sel v]"
combo, header+visible-rows list). Flagship carrier example/ui/gallery/gallery.cb starts
here (each control authored via <Tag/> sugar), self-test 8/8 leak-clean AND asan-clean
including a 100k-row virtualization assert (rowText fired <100x) + selection/activation
round-trips. Cocoa + WinUI hosts got setListOp stubs (free the box, real backends -> P11).

Deviations / notes:
- The gallery imports win32_native_host.cb directly (Windows-only, like
  win32_native_settings) - its SendMessage-driven self-test is inherently Win32; P11
  ports it (and the CocoaHost/WinUI3Host setListOp bodies) to the other hosts.
- NO compiler changes. Found + filed a real compiler bug:
  internal/issue/negative-int-literal-global-init.md - a negative integer literal in a
  GLOBAL/const initializer is truncated to minimal unsigned width (`const int x = -150`
  reads back 65386); local inits / returns are fine. Worked around with `0 - N` for the
  LVN_*/NM_* notify codes (folds correctly). Reported prominently.
- Gates: gallery 8/8 leak+asan-clean, win32_native_settings 8/8, fedit 10/10 leak-clean,
  example.bat Release 85/0/26, test.bat Release all pass, cocoa + winui hosts
  `--check` green (cocoa via `--platform macos`).

Original spec (for reference):

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

DONE 2026-07-07. Four tier-2 navigation controls landed behind the unchanged NativeHost
property surface, reusing the P8 setListOp seam (no new interface method - plan risk #3):
TabControl/TabPane (WC_TABCONTROL header strip; keyed panes with LAZY inactive tabs - only
the active pane maps to native, so per-tab buffers must live in the app model; controlled
selectedTab + LISTOP_TAB_RESET/ADD/SET_SEL), TreeView (WC_TREEVIEW; expand-on-demand node
source = three callbacks keyed by an int nodeId, boxed as a u64 TreeBox* like ListRowBox,
TVN_ITEMEXPANDING materializes children lazily; LISTOP_TREE_SET_SRC_CB/REBUILD, selection
round-trips via TVN_SELCHANGED), SplitView (a layout container - the layout engine learned
the weighted two-pane constraint + a 1-DIP divider gutter; Win32 hit-tests the gutter in
the parent WndProc with SetCapture, nativeSplitterDrag drives it headlessly), and a
per-element ContextMenu (reuses the P3 declarative menu model; host-owned, registered by
key via nativeSetContextMenu, shown with TrackPopupMenu, routes the chosen cmd through the
app menu handler). Toolbar is a documented pattern, not a new element (toolbar() = a
DIR_ROW View of Buttons; icons arrive with Image in P10). Canvas fallbacks render for all
(tabs -> header row + active pane, tree -> indented rows, split -> two panes + divider).
Reconcile learned adoptContainerProps to copy the controlled container scalars
(selectedTab / ratio) onto the retained committed node (the RadioGroup leaf-prop pattern
does not apply to geometry-driving container scalars).

FLAGSHIP: fedit v2 (example/ui/fedit/fedit.cb) - file-tree sidebar (TreeView over a fake
in-memory project), multi-tab documents (TabControl; a shared native EDIT + per-tab
buffers parked in the app model, so tab switch preserves each buffer), a splitter between
tree and editor, a context menu on the tree, and a toolbar. Self-test 14/14 leak+asan-clean
(adds: tab-switch buffer preservation, tree expand-on-demand + select opens a file into a
tab, splitter drag changes layout, context-menu command routes) on top of the P7 six.
gallery.cb extended to 14/14 (adds TabControl/TreeView/SplitView/ContextMenu pages).

Deviations / notes:
- SplitView is a layout CONTAINER (not a native control), like RadioGroup - the divider is
  a gap the parent WndProc hit-tests (no subclassed child), which is simpler and robust.
  Cocoa's NSSplitView is P11.
- Per-tab editor buffers live in the app MODEL (parallel list<string>), not inactive
  native controls: "only the active pane reconciles to native" (plan spec) and "tab switch
  preserves buffers" (gate) are in tension for a real editor, and the model-owned buffer +
  shared EDIT is how real single-editor multi-tab editors work anyway.
- ContextMenu is a model object (chrome), authored via <ContextMenu/> sugar in a builder
  that returns an owned ptr the caller inline-casts to u64; the host owns + frees it. The
  three new ELEMENTS (TabControl/TreeView/SplitView) are each authored via <Tag/> sugar in
  the gallery self-test.
- NO compiler changes. Found + filed a real compiler bug:
  internal/issue/list-add-set-owned-string-move-leaks.md - a BARE owned string (call
  result or named local) moved into list<string>.add/set leaks its buffer (owned by
  nobody: source marked moved-from, slot never adopts it). Wrapping the arg in `"" + ...`
  is the fix (a concat temp is adopted); string literals were always safe, which is why
  the gallery's `add("" + s)` never leaked. Worked around in fedit's tab bookkeeping;
  reported prominently. Also re-hit the known "owned string TEMP passed straight as a
  read-only string call-arg leaks" pattern (bind to a named local first - editorHas helper).
- Cocoa + WinUI hosts got placeholder controls (NSTextField / TextBlock) + the tab/tree
  setListOp box-free + host-neutral driver STUBS so shared app code compiles (fedit
  `--check --platform macos` PASS; winui_demo `--check` PASS); real NSTabView/NSOutlineView/
  NSSplitView + WinUI TabView/TreeView backends are the P11 parity sweep. The gallery imports
  win32_native_host.cb directly (Windows-only, like win32_native_settings), so its
  SendMessage-driven self-test is inherently Win32; the fedit P9 self-test asserts are
  `if const (!__MACOS__)`-gated (name-resolved on mac, never run there).
- Gates: gallery 14/14 leak+asan-clean, fedit 14/14 leak+asan-clean, win32_native_settings
  8/8, example.bat Release 85/0/26, test.bat Release all pass, cocoa + winui `--check` green.

Original spec (for reference):

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

DONE 2026-07-07. Three elements complete the set, plus the deferred accent buttons and the
flagship gallery. **Image** (ELEM_IMAGE): a leaf carrying a BORROWED top-down BGRA32 buffer
(pixels + pxW/pxH) that the host uploads via a NEW `setImageData(h, pixels, w, h, stride)` seam
method (top-down DIB section + STM_SETIMAGE, HBITMAP bookkeeping freed on destroy/dispose); a
`.bmp source` path decodes host-internally via LoadImage. **GroupBox** (ELEM_GROUPBOX): a titled
BS_GROUPBOX frame whose children position as siblings. **CanvasView** (ELEM_CANVAS): the escape
hatch - a GDI-backed child window (CFlatCanvasChild + NativeGdiCanvas implementing the ui.cb
Canvas) whose WM_PAINT/WM_PRINTCLIENT invokes the app's boxed `onPaint(Canvas)` closure (a
CanvasBox opaque-u64, like ListRowBox). **NM_CUSTOMDRAW accent buttons**: a themed push button
fills with theme.buttonBg + focus ring via NM_CUSTOMDRAW (setAccent stores per-control colors;
monochrome themes store none -> native look, CDRF_DODEFAULT). **Flagship gallery.cb** now
authors EVERY element (forms + data + nav + visuals) via `<Tag/>` sugar, a light/dark toggle,
a 25-assert headless self-test, and `--shots <dir>` -> PrintWindow light/dark BMPs. Each new
element has a Canvas fallback (image -> placeholder+altText, groupbox -> bordered box+title,
canvasview -> delegate to onPaint). Cocoa + WinUI hosts got setImageData stubs + placeholder
controls (real NSImageView/NSBox/CGView + WriteableBitmap -> P11).

Deviations / notes:
- `setImageData` is a NEW NativeHost interface method (not an overloaded setListOp op) - a raw
  pixel-pointer + w/h/stride operand is unlike the LISTOP_* integer/string/box operands, and
  image data never rides the item-data virtualization path. Documented at the interface.
- Decoder: the toolkit-neutral BGRA32 pixel seam is fully implemented (the gallery pushes a
  procedural gradient through it, proving it end to end); file decode uses `LoadImage` for `.bmp`.
  WIC-based PNG/JPG decode (the plan's "WIC" wording) is the SAME seam with a richer host-internal
  decoder, deferred as an enrichment - it needs no seam or element change.
- NM_CUSTOMDRAW cannot be captured by PrintWindow (it re-renders controls in their default style),
  so the accent fill is a LIVE-render feature. It is verified headlessly by a pixel-level draw
  assert (`nativeTestCustomDrawFill` drives the custom-draw path into a memory DC and checks the
  filled pixel equals the stored accent), NOT by the screenshot. The dark-mode BMP therefore shows
  default-styled buttons; the accent is real on-screen (NM_CUSTOMDRAW fires on paint, confirmed).
- The gallery stays SINGLE-COLUMN (preserves the P8/P9 self-test key paths; keys are order-
  independent, so the P10 visuals were placed high to land in the screenshot). It is taller than
  one screen, so `--shots` captures the top band + the Image/CanvasView visuals; the FULL element
  set is covered by `--selftest`, not the screenshot (the capture pre-fills the theme bg so any
  unrendered tail is not black). fedit toolbar icons via Image were left out (optional, small).
- NO compiler changes. The interface-typed lambda param `Lambda<void(Canvas)>` (CanvasBox) was
  probed first and compiles + runs asan-clean. The known compiler pitfalls were worked around as
  before (negative-int-literal `0 - N` for NM_CUSTOMDRAW, named-borrow downcasts, `"" +` string
  concats); no new issue filed. Nine of the P10 Win32 constants (STM_SETIMAGE, SS_BITMAP,
  BS_GROUPBOX, GWLP_USERDATA, ...) are already surfaced by the windows.h binding - only the
  NM_/CDDS_/CDRF_/CDIS_ custom-draw codes are declared by hand.
- Gates: gallery 25/25 leak-clean AND asan-clean (incl. the pixel-level accent-draw assert),
  screenshots written (gallery_{light,dark}.bmp, ~2.9 MB each, visually verified), fedit 14/14
  leak-clean, win32_native_settings 8/8 leak-clean, example.bat Release 85/0/26, test.bat Release
  all pass, cocoa (fedit + cocoa_native_settings) + winui_demo `--check` green.

Original spec (for reference):

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

### P11 - macOS parity sweep (Cocoa backends for everything P7-P10 added)

DONE 2026-07-07, in the two parts the EXECUTION NOTE called for: (a) the gallery
host-neutral driver port, FULLY VERIFIED ON WINDOWS; (b) the Cocoa backends for the
P7-P10 stubs, authored best-effort and gated on `--check --platform macos` ONLY.
Mac RUNTIME verification is DEFERRED (no arm64 box available) and must happen before
P14 promotion signs off on mac.

Part (a) - gallery host-neutral driver port (verified on Windows):
- `example/ui/gallery/gallery.cb` now imports the `native_host.cb` shim (was a direct
  `win32_native_host.cb` import), so it compiles against either backend like fedit.
- Every SendMessage-driven assert became a host-neutral `native*` test driver on the
  NativeHost surface. New drivers added to BOTH hosts (design is host-agnostic so the P12
  WinUI sweep reuses them): `nativeClickButton`, `nativeIsChecked`, `nativeComboSelected`,
  `nativeComboSelect`, `nativeTypeText`, `nativeListActivate`, `nativeProgressValue`,
  `nativeSliderSet`, `nativeImageHasBitmap`, `nativeAccentBg`. The existing
  `nativeList*`/`nativeTab*`/`nativeTree*`/`nativeSplitterDrag`/`nativeStatusText`/
  `nativeTestCustomDrawFill`/`hostDark`/`startHeadlessWindow` set carries the rest. Each
  routes through the same element-model handler the OS fires, then re-renders.
- The Win32-only screenshot path (PrintWindow -> BMP, `saveWindowBmp`/`galleryShots` + the
  bitmap import + PrintWindow/GetDIBits externs) is gated out of the Cocoa build at file
  scope. Documented in doc/UI.md "Host-neutral test drivers" (API bumped to v16).

Part (b) - Cocoa backends (compile-checked, runtime deferred):
- **setListOp bodies**: ComboBox -> `NSPopUpButton` (populate/select/read); ListView ->
  `NSScrollView` + `NSTableView` whose dataSource (the shared `CfHostTarget`) answers
  `numberOfRowsInTableView:` / `tableView:objectValueForTableColumn:row:` from the boxed
  `ListRowBox` (naturally virtualized); TreeView -> `NSScrollView` + `NSOutlineView` whose
  dataSource resolves the `TreeBox` callbacks with items = `NSNumber(nodeId)` (expand-on-
  demand via `numberOfChildrenOfItem:`/`child:ofItem:`/`isItemExpandable:`); TabControl ->
  `NSSegmentedControl` strip + the existing lazy-active-pane walk.
- **Visuals**: setImageData -> `NSImageView` fed an `NSBitmapImageRep` copied from the
  borrowed BGRA32 buffer; GroupBox -> titled `NSBox`; CanvasView -> `CfCanvasView` (a
  flipped custom NSView) whose `drawRect:` bridges the boxed `onPaint(Canvas)` through
  `CocoaCanvas`, a Canvas impl over NSBezierPath/NSColor/NSString drawing; accents ->
  `bezelColor` + `contentTintColor` (look policy).
- **ctx.post marshaling**: real `performSelectorOnMainThread:` onto `CfHostTarget`
  `runPostBox:` (the u64 box wrapped in an NSNumber), same clone-in/destruct-after-call
  contract; bound in `createNativeWindow` via `ctx.bindPost`/`bindThreadId`.
- **Chrome**: per-element context menus stored host-side by key and routed through the app
  menu handler by `nativeFireContextMenu` (mirror of Win32); StatusBar/tooltip mappings
  from P7 unchanged.
- New reusable objc bridge helpers in `example/macos/cocoa.cb` (dataSource-shaped
  `addMethod*` variants, `nsColor`, `nsBitmapRep`, `msgPerformOnMain`, `msgGetI64Arg`,
  `msgIntArg`). Box lifetimes tracked in `CocoaHost` parallel lists and freed on
  `destroyControl` + `nativeTeardownForTest` (mirror of the Win32 host).

Deviations / notes:
- **Multi-window on Cocoa stays single-window** (the Win32 `Window`-class multi-window
  model was NOT mirrored). fedit's "New Window" self-test is `if const (!__MACOS__)`-gated,
  so this does not block the mac compile gate; real multi-window on Cocoa is carried into
  P13/P14 hardening. Documented honestly rather than faked.
- **SplitView** stays a layout container (parent-tracked divider gap, same
  `nativeSplitterDrag` driver), NOT `NSSplitView` - keeps the layout model single-source
  per the plan.
- **Accents** on Cocoa ride `bezelColor`, so `nativeAccentBg`/`nativeTestCustomDrawFill`
  return 0 there (no stored COLORREF); the gallery's accent assert is a Win32 live-render
  check in spirit.
- **`if const` gotcha (found + worked around, not a compiler bug)**: the ForwardRefScanner
  only folds a SIMPLE-identifier `if const` condition, so a negated `if const (!__MACOS__)`
  type-scans BOTH branches (unknown Win32 types leak through). The Win32-only screenshot
  code is therefore gated as `if const (__MACOS__) { } else { ... }`, whose `else` the
  scanner correctly skips under `--platform macos`. (The fedit P9 asserts under
  `if const (!__MACOS__)` only reference host-neutral names, which resolve on both, so they
  were unaffected and stay as-is.)
- NO compiler changes. NO new NativeHost interface methods (the driver additions are host
  free functions; the seam surface is unchanged, so the WinUI host is untouched).
- The three previously-filed UI compiler bugs are fixed on master (negative-int global
  init, list<string>.add/set move-leak, downcast-of-call-result double-free); none were
  re-hit. The known "owned-string temp as a read-only call-arg leaks" pitfall was avoided
  by binding status reads to named locals in the gallery helpers.

Gates:
- Windows: `example.bat Release` 85/0/26, `test.bat Release` all pass. gallery self-test
  25/25 leak-clean (--heap-audit) AND asan-clean (--asan); fedit 14/14 + win32_native_settings
  8/8 leak-clean; gallery `--shots` still writes light/dark BMPs.
- macOS (compile-check only, runtime DEFERRED): `--check --platform macos` green for
  `gallery.cb`, `fedit.cb`, and `cocoa_native_settings.cb`
  (`-i example/ui -i example/macos --platform macos`).

Original spec (for reference):
Goal: gallery + fedit v2 run natively on the arm64 mac; every P7-P10 Cocoa
stub becomes a real backend. This is the older and larger of the two parity
debts, and doing it first hardens the host-neutral self-test drivers that
the WinUI sweep (P12) then reuses.

- **App shell (P7 debt)**: real multi-window on Cocoa (mirror the Win32
  `Window` class over the P7 host-neutral accessors; one NSApplication run
  loop, list of windows) and real ctx.post() marshaling
  (performSelectorOnMainThread: or dispatch_async onto the main queue,
  same boxed-closure u64 contract; clone-in, destruct-after-call under the
  shared nativeTeardownForTest leak gate).
- **setListOp bodies (P8/P9 debt)**: ListView -> NSTableView driven by a
  dataSource that queries the existing rowText ListRowBox (NSTableView is
  naturally virtualized - this is the payoff of the P8 callback-source
  design); TreeView -> NSOutlineView with expand-on-demand through the
  TreeBox callbacks; TabControl -> tabless NSTabView or segmented control +
  pane swap (decide in-milestone, per the P9 note; lazy-inactive-panes
  contract unchanged); ComboBox -> NSPopUpButton.
- **SplitView**: keep the shared layout-container divider (parent-view
  mouse tracking, same nativeSplitterDrag headless driver) rather than
  NSSplitView, so the layout model stays single-source; fall back to
  NSSplitView only if event tracking fights the flipped view.
- **Visuals (P10 debt)**: setImageData -> NSImageView (CGImage wrapped
  around the borrowed BGRA32 buffer), GroupBox -> titled NSBox, CanvasView
  -> custom NSView whose drawRect: bridges into the boxed onPaint(Canvas)
  via a CG-backed Canvas impl, accents -> bezelColor/NSAppearance only
  (look policy).
- **Chrome**: per-element context menus (NSMenu popUpMenuPositioningItem
  through the same nativeSetContextMenu registration); StatusBar/tooltip
  mappings exist from P7 - verify at runtime. Mac conventions audited once:
  app menu, menu-bar-first, Ctrl->Cmd accelerator map coverage.
- **Gallery goes cross-platform**: port gallery.cb off its direct
  win32_native_host.cb import onto the native_host.cb shim. Its
  SendMessage-driven asserts must become host-neutral test drivers on the
  NativeHost surface (the nativeSplitterDrag/nativeStatusText pattern) -
  this driver set is the piece P12 reuses, so design it host-agnostic.
- Gate: example_mac.sh extended - gallery + fedit v2 self-tests green ON
  THE ARM64 BOX (leak gate = shared nativeTeardownForTest path; HeapAudit
  is Windows-only), test.sh + test_lsp still green there; Windows stays
  green and byte-identical (test.bat + example.bat Release; Win32 host
  churn limited to the gallery driver refactor).
- Risk: needs physical access to the mac box for every runtime gate;
  `--check --platform macos` from Windows only guards compilation.
- EXECUTION NOTE 2026-07-07: no mac box available right now (user). P11
  executes in two parts: (a) the gallery host-neutral driver port, fully
  verified on Windows; (b) the Cocoa backends authored best-effort and
  gated on `--check --platform macos` ONLY - runtime verification on the
  arm64 box is DEFERRED and must happen before P14 promotion signs off
  on mac.

### P12 - WinUI 3 parity sweep (the M3 deferred list + the rich set)

DONE 2026-07-07. The widget-gallery self-test runs GREEN on WinUI3Host (25/25, headless),
which validates the setListOp seam a THIRD time and retires the last pre-freeze design risk.
NO compiler changes were needed; every WinRT projection was de-risked with spikes first.

What landed:
- **winui_host.cb rewritten** from the 4-element M3 proof to the FULL element set behind the
  unchanged NativeHost seam: TextBox + TextArea (uncontrolled-with-sync), Slider + ProgressBar
  (IRangeBase put/get Value; Maximum set before Value since a template-less RangeBase defaults
  Maximum to 1.0), RadioButton (controlled via the model), ComboBox (live IItemsControl.Items as
  IVector<object> + ISelector SelectedIndex), ListView, TabView, TreeView, Image (WriteableBitmap),
  headered GroupBox, StatusBar, and a CanvasView placeholder. Composable controls that return
  E_NOTIMPL from RoActivateInstance (ProgressBar/TabView/TreeView) are built via their
  IXxxFactory::CreateInstance(null outer) - the same null-outer composition the base Application
  and Window use.
- **setImageData -> WriteableBitmap**: CreateInstanceWithDimensions, then the BGRA32 buffer is
  copied row-by-row (honoring stride) into PixelBuffer through IBufferByteAccess. That interface is
  classic COM (not WinRT), so iidof cannot supply its IID; it is HAND-DECLARED (a `function<...>`
  vtable like core/com.cb's IUnknown) with a hand-built GUID, and the WriteableBitmap is set as the
  Image.Source.
- **The full native* driver surface** (the P11 host-neutral set) on WinUI3Host: click/check/combo/
  type/list/tab/tree/splitter/progress/slider/image/statusbar/accent/context-menu. The data-control
  drivers answer from the host-side setListOp box/model (the rowText ListRowBox + a TreeBox-backed
  materialized-node model), routing through the same element-model handlers the OS fires, then
  re-rendering via pumpWinui - identical driver contract to Win32/Cocoa.
- **ctx.post** wired to DispatcherQueue.TryEnqueue via winrtDelegate (same boxed-closure opaque-u64
  contract, __uiRunPostBox on the UI thread); bound in winuiInitFramework.
- **The gallery went host-neutral by SPLIT**: the GalleryApp Component + the 25-assert self-test
  moved to `example/ui/gallery/gallery_app.cb` (imports only ui.cb; no host). Its Win32/Cocoa
  launcher stays `gallery.cb` (imports native_host.cb + gallery_app.cb, keeps main + the Win32
  screenshot path); the WinUI launcher is `example/ui/winui/winui_gallery.cb` (imports winui_host.cb
  + gallery_app.cb). A cflat compilation shares one global scope across its whole import closure, so
  gallery_app.cb resolves the host drivers from whichever launcher also imports a host (verified
  first with a micro-test). This is the "native_host shim vs thin launcher" choice from the plan -
  resolved as a thin launcher over a shared core, because cflat has no CLI --define to switch the
  backend inside the shim.
- **Headless self-test structure**: the whole gallerySelfTest runs SYNCHRONOUSLY inside
  Application.Start's init callback (runWinuiHeadless installs it via setSelfTestBody). The XAML
  framework is up there, so startHeadlessWindow mounts the tree and the drivers create/drive/read
  controls with plain COM calls (no reposting/UIA needed - that is why winui_demo's reposting state
  machine is not required here). It then Exits and the launcher returns the rc.

Deviations / documented gaps (all in the doc/UI.md v17 parity matrix, none silent):
- **CanvasView is a placeholder Border** - Win2D would be a NEW dependency against the
  self-contained-exe pitch, so per the plan's default decision the escape hatch stays Win32/Cocoa.
  The onPaint closure is still owned + freed, never invoked.
- **GroupBox is a header label** (a look difference), not a titled frame.
- **ListView/TabView/TreeView are driver-backed**: the real XAML control is created (keyed identity
  + _exists hold), but its item DISPLAY is answered from the host setListOp box/model rather than a
  live virtualized ItemsSource (which needs a WinRT bindable-collection implementation - deferred as
  a visual enrichment). The seam + the rowText/TreeBox callbacks are fully exercised; the 100k-row
  virtualization oracle passes (rowText fires only for queried cells).
- **Accents**: WinUI has no NM_CUSTOMDRAW, so setAccent stores the accent (a brush intent) and BOTH
  nativeAccentBg and nativeTestCustomDrawFill return that stored color - the check is "reported ==
  stored", no offscreen pixel readback. The gallery's accent assert passes unchanged (no per-host
  gating was needed - the driver semantics satisfy it honestly).
- **Multi-window stays single-window** (mirrors the Cocoa P11 decision); the gallery gate needs one
  window and fedit-on-WinUI was explicitly non-gating. Real multi-window on WinUI is carried forward.
- **tooltip** is not wired to ToolTipService (no self-test covers it on WinUI).
- **fedit v2 on WinUI** was the stretch/non-gating item and was NOT attempted.
- A latent gotcha found + worked around (not a compiler bug): `WinUI3Host _wHost = default` zeroes
  field initializers, so the tree item-handle sequence (`= 1u`) started at 0 and collided with the
  "parent = 0 = top-level" sentinel; seeded to 1 lazily on first use so handles are never 0.

Gates (all green):
- example.bat Release: 86 passed / 0 failed / 28 skipped (was 85/0/26; +1 pass = the winui_gallery
  worker, +2 skip = gallery_app + winui_gallery excluded from the plain-file sweep). winui_gallery,
  winui_demo, winui_app_demo all PASS (winui self-tests run WITHOUT --heap-audit, per example.bat).
- test.bat Release: all tests passed.
- WinUI gallery self-test: 25/25 headless, stable across repeated runs.
- Win32 unaffected: gallery 25/25, fedit 14/14, win32_native_settings 8/8, all leak-clean.
- macOS compile gate: --check --platform macos green for gallery.cb, fedit.cb, cocoa_native_settings.cb.

Original spec (for reference):
Goal: gallery runs on WinUI3Host; the setListOp seam is validated a third
time, which is the last design risk before the API freezes.

- **M3 deferred list first** (forms parity): TextBox (same
  uncontrolled-with-sync TextArea contract), Slider/ProgressBar via
  IRangeBase, theme accents (resource/brush-based - documented APIs only),
  multi-window (one Window object per app window on the shared dispatcher,
  riding the P7 Window-list model).
- **Data + navigation set**: ListView -> ItemsView/ListView with the
  rowText box as the item source (the third seam validation); ComboBox;
  TabControl -> TabView; TreeView -> WinUI TreeView with expand-on-demand;
  context menus -> MenuFlyout; StatusBar -> bottom-docked bar; tooltips ->
  ToolTipService.
- **ctx.post()**: DispatcherQueue.TryEnqueue through winrtDelegate (the
  P6 M1 builtin already proves the closure-to-COM-callable path).
- **Visuals**: Image -> WriteableBitmap fed from the borrowed BGRA32
  buffer; GroupBox -> headered/bordered panel approximation (document as a
  deliberate look difference); CanvasView -> DECIDE IN-MILESTONE: Win2D is
  a new dependency (against the self-contained pitch), so the default is a
  documented deliberate gap (escape hatch stays Win32/Cocoa) unless a
  dependency-free composition path proves cheap.
- Gate: gallery self-test green on WinUI3Host via the P11 host-neutral
  drivers + UIA where needed (still WITHOUT --heap-audit - WinAppSDK
  process-lived singletons, documented in example.bat); fedit v2 optional
  on WinUI (stretch, not gating); parity matrix table added to doc/UI.md
  (element x host, deliberate gaps documented, not silent); test.bat +
  example.bat Release green.

### P13 - hardening + API-freeze prep (the v1.0-readiness milestone)

DONE 2026-07-07. The pre-freeze hardening pass landed with NO compiler changes and NO
element/seam surface changes; doc/UI.md is versioned v1.0-rc. What landed:

- **Compiler-workaround removal sweep.** All three now-fixed-on-master bugs' workarounds
  were removed from example/ui and the natural spellings restored, and the sweep is gated
  green under BOTH --heap-audit and --asan on the Win32 gallery + fedit (+ leak-clean on
  win32_native_settings). Tally: (1) negative-int global-init `0 - N` -> plain negative
  literals on the 9 FILE-SCOPE const negative initializers (win32_native_host.cb LVN_*/NM_*/
  TCN_*/TVN_*/NM_CUSTOMDRAW = 8, ui.cb TREE_ROOT = 1). LEFT AS-IS with reason: struct-field
  default initializers (`int selectedNode = 0 - 1`) and in-expression `(0 - 1)` sentinels /
  return values in winui/cocoa hosts - those were never the global-init bug (locals/returns/
  field-defaults sign-extend correctly), so touching them is out of scope. (2) list<string>
  move-leak `"" + owned` -> restored 1 site (fedit `_saveActiveToModel` `tabBuffers.set(...,
  nativeControlText(...))`, an owned call result). LEFT AS-IS with reason: every other `"" +`
  in the tree is a LEGITIMATE owned-copy-from-borrowed conversion, not the list-move
  workaround - `"" + this.path`/`"" + path` (copy a live field/param so a move into the slot
  does not zero it), the P7 `(string)buf`/`"" + buf` stack-buffer UAF fixes, and the
  `"" + key`/`keyOf()` borrowed-param copies. (3) two-step named-local downcast -> collapsed 6
  `Component app = activeApp(); T* p = app as T;` pairs to direct `T* p = activeApp() as T;`
  (fedit x3, gallery.cb x1, gallery_app.cb x2). Confirmed the three internal/issue files are
  gone (the whole internal/issue/ dir had been removed when the last was fixed).
- **Keyboard + focus audit + minimal accessibility.** Audited: every Win32 control is
  `WS_TABSTOP` and parented FLAT to the top-level window (GroupBox/SplitView/RadioGroup are
  layout containers, not nested HWNDs), so `IsDialogMessageA` Tabs across the whole set
  including ListView/TreeView/TabControl/ComboBox with NO `WS_EX_CONTROLPARENT` needed
  (nothing is nested). Enter/Space/arrow keys are stock common-control behavior; dialog Esc/
  Enter is the OS MessageBox convention. Added a Win32-only headless focus-traversal readback
  (`nativeFocusFirstH`/`nativeFocusNextH`/`nativeFocusedKey`, walked by handle so it is
  leak-free) asserting the Tab ring closes over >= 8 distinct controls. Accessibility (minimal,
  name-level): text-bearing controls already expose their window text as the MSAA/UIA Name; for
  textless controls (Progress/Slider/List/Tree/Combo/Image/Canvas) the Win32 host now mirrors a
  present `tooltip` into the (unpainted) window text so a screen reader announces a Name -
  verified by a `nativeAccessibleName` readback. Documented in doc/UI.md "Accessibility" +
  "Keyboard and focus"; parity matrix gained an "a11y Name" row.
- **Relayout + theming hardening.** Verified the existing WM_SIZE relayout (recompute DIP w/h
  -> buildCurrentTree, with a 1-DIP min-size floor) and WM_DPICHANGED (re-font every live
  control + resize + relayout) cover the full P8-P10 control set. Added a Win32 headless
  live-resize assert (widen -> root relayouts to the new width; a 1x1 clamp does not crash and
  controls survive) and a host-neutral **theme-flip storm** gallery assert (26 light/dark
  toggles, host titlebar stays in lockstep with the model each reconcile), both leak- and
  asan-clean.
- **Reconcile stress-soak.** Added a host-neutral seeded-LCG stress mode to GalleryApp (a
  "stress-step" button + a churn box) and gallery self-test assert #27: 48 deterministic steps
  of create/destroy/reorder across an 8-kind churn pool (Button/Text/Checkbox/TextInput/
  Progress/Slider/GroupBox/ComboBox); after each step the native control census (one live handle
  per key) must exactly match this.stressMask, and a slot present before AND after keeps its
  handle (identity preserved across the reorder). Teardown leak- and asan-clean.
- **API consistency sweep -> doc/UI.md v1.0-rc.** One editing pass: added the event-naming
  convention table (the `onChange` vs `onChangeText` vs `onSelect`/`onActivate` rationale -
  single-scalar controls use onChange, dual-interaction controls name select/activate), the
  controlled-vs-uncontrolled table, the a11y + keyboard sections, and the hardening self-test
  section; bumped the title + version note to v1.0-rc (API v18). NO renames were applied - the
  convention is principled as-is and every rename would be a breaking change for a cosmetic gain
  (documented as-is per plan).

Deviations / notes:
- **NO element/seam/compiler changes.** All P13 work is additive .cb + docs. The a11y change is
  a host-internal SetWindowTextA (name-only, no visual change); the new focus/a11y/resize
  drivers are Win32-only free functions gated out of the Cocoa build (`if const (__MACOS__){}
  else{...}`), so the NativeHost seam is unchanged and WinUI/Cocoa are untouched by them.
- **Stress-soak churn pool = 8 robust kinds**, not literally "every elemKind": the data/nav/
  visual kinds (List/Tree/Tabs/Image/Canvas) are reconciled every step by the STANDING gallery
  tree anyway; the churn pool focuses the create/destroy/reorder fuzzing on the leaf/container/
  data families that are cheap to construct valid instances of, which keeps the shared self-test
  robust on all three hosts (it runs green on WinUI 3 too, 27/27).
- **macOS RUNTIME still deferred** - the whole P13 pass is `--check --platform macos` green for
  gallery/fedit/cocoa_native_settings; runtime verification on an arm64 box is carried into P14
  and MUST precede the mac sign-off.
- **NEW compiler bug found + filed + worked around (not fixed - no compiler changes allowed):**
  internal/issue/return-owned-call-result-directly-leaks.md - `return someCall();` where the
  callee returns an owning string LEAKS (the caller's bound local does not adopt it); binding
  first (`T k = someCall(); return k;`) is clean. Minimal repro confirmed under --heap-audit.
  Same family as the noted ternary-owned-return leak. Worked around in the P13 focus drivers
  (bind-then-return). Reported prominently.

Framework name candidates (P13 settles the name; the USER decides; P14 applies it mechanically).
Constraints checked: ASCII, valid as `import "NAME.cb"`, no clash with existing core/ modules
(core/ has `view.cb`, so "view" is OUT; no ui/gui/widget/forms/native/panel/shell exist):
- **`weave`** (RECOMMENDED) - the keyed reconciler weaves a declarative Element tree onto native
  OS controls; short, distinctive, memorable, zero clash. `import "weave.cb";`
- **`native_ui`** - maximally descriptive + discoverable; the conservative fallback if an
  evocative name is unwanted. Slightly long/generic. `import "native_ui.cb";`
- **`facet`** - each native control is a facet of one declarative surface; clean UI connotation,
  short. `import "facet.cb";`
- **`loom`** - same weaving metaphor as weave, even shorter; minor risk of collision with common
  product names. `import "loom.cb";`
- **`sprig`** - small/lightweight, leaning into the anti-Electron ("not a browser") pitch;
  playful and unambiguous. `import "sprig.cb";`
Recommendation: **`weave`**, with **`native_ui`** as the safe fallback.

Gates (all green):
- example.bat Release: 86 passed / 0 failed / 28 skipped (unchanged from the P12 baseline - the
  new asserts live inside existing workers, adding no plain-file entries).
- test.bat Release: all tests passed.
- Win32: gallery self-test 27/27 (25 element + theme-storm + stress-soak) AND win32 hardening
  4/4 (focus, relayout, a11y), leak-clean (--heap-audit) AND asan-clean (--asan); fedit 14/14
  leak+asan-clean; win32_native_settings 8/8 leak-clean.
- WinUI 3: gallery self-test 27/27 (theme-storm + stress-soak run green on WinUI too).
- macOS compile gate: --check --platform macos green for gallery.cb, fedit.cb,
  cocoa_native_settings.cb.

Original spec (for reference):

Goal: everything that should be true BEFORE the API freezes and the files
move - so P14 promotion is mechanical, not a stabilization effort.

- **Compiler-workaround removal sweep**: the three bugs the UI work filed
  are now fixed on master (negative-int-literal global init 4a8bf29,
  list<string>.add/set move-leak d8b4f3e, downcast-of-call-result
  double-free 6cdc63d). Sweep example/ui/ + hosts for the `0 - N` notify
  codes, the `"" + s` adoption concats, and the two-step named-local
  downcasts; restore the natural spellings; confirm the matching
  internal/issue/ files are deleted. Gate under --heap-audit + asan so any
  regression in the fixes surfaces here, not after promotion.
- **Keyboard + focus audit**: Tab order across the FULL element set on all
  three hosts (IsDialogMessageA covers Win32 forms - verify it against
  ListView/TreeView/TabControl), Esc/Enter conventions in dialogs,
  accelerator-conflict check. Minimal accessibility pass promoted from the
  old stretch list: UIA/NSAccessibility names sourced from text/tooltip
  props - native a11y is a headline anti-Electron claim, so v1.0 should
  demonstrably have it.
- **Relayout + theming hardening**: live window-resize relayout with the
  full P8-P10 element set, min-size clamps, WM_DPICHANGED re-verified at
  200% against the new controls, and a light/dark toggle storm (gallery
  theme flip x N) leak-clean + asan-clean.
- **Reconcile stress soak**: a seeded random tree-mutation self-test mode
  in the gallery (create/destroy/reorder keyed elements across every
  elemKind, verify native-control census + teardown leak-clean per
  iteration) - guards the keyed-diff core with fuzz-style coverage before
  the freeze, in the spirit of the HeapAudit fuzzing oracle.
- **API consistency sweep + naming**: one editing pass over doc/UI.md
  (prop naming, event naming, controlled vs uncontrolled table, the parity
  matrix from P12) producing the frozen v1.0 surface; settle the framework
  NAME here (moved up from promotion) so P14 renames once, mechanically.
- Gate: all self-tests green on three hosts (Win32 full, Cocoa via
  example_mac.sh, WinUI per its documented gating), test.bat + test_lsp +
  example.bat Release green, no remaining workaround markers in
  example/ui/, doc/UI.md reviewed and versioned v1.0-rc.

### P14 - promotion to core/ (the "complete framework" stamp)

DONE 2026-07-07. The framework + all hosts were promoted into `core/` under the
USER-CHOSEN name **`ui_native`** (not the plan's recommended `weave`), and every
consumer + doc + test harness rewired to the new names. NO compiler changes; NO
element/seam changes. What landed:

- **Seam-merge decision.** The chosen name `ui_native` collided with the old seam
  filename `example/ui/ui_native.cb`, so the framework file (`ui.cb`) and the seam
  file (the old `ui_native.cb`, which did `import "ui.cb";`) were MERGED into a single
  `core/ui_native.cb` (framework body + the seam's consts/`NativeHost` interface
  appended, its `import "ui.cb";` dropped). This is the one module apps import for the
  Canvas hosts.
- **File layout in `core/`:** `ui_native.cb` (merged framework + seam),
  `ui_native_host.cb` (<- `native_host.cb`, the `if const` platform shim, imports
  rewritten to `ui_native_cocoa.cb`/`ui_native_win32.cb`), `ui_native_win32.cb` (<-
  `win32_native_host.cb`), `ui_native_cocoa.cb` (<- `cocoa_native_host.cb`),
  `ui_native_winui.cb` (<- `winui/winui_host.cb`, its `ui.cb`+`ui_native.cb` imports
  collapsed to the single merged `ui_native.cb`), and `cocoa.cb` (<- `example/macos/
  cocoa.cb`, the objc bridge the Cocoa host depends on).
- **Consumers stayed in `example/`** with imports rewired: the Canvas demos + gallery
  component (`import "ui.cb"` -> `"ui_native.cb"`), the settings probes (`win32_native_host`
  -> `ui_native_win32`, `cocoa_native_host` -> `ui_native_cocoa`), fedit + gallery
  (`native_host` -> `ui_native_host`), and the winui launchers (`winui_host` ->
  `ui_native_winui`). Core imports now resolve with NO `-i` (verified:
  `cflat example/ui/fedit/fedit.cb -o out/fedit.exe`).
- **Build system.** CMake globs `cflat/core/*` (CONFIGURE_DEPENDS) and deploys the tree
  next to the exe, so the new files are picked up automatically - no build-file edit.
  (The legacy `cflat.vcxproj` no longer exists; the build is CMake-only. Updated the
  stale "add to cflat.vcxproj" note in internal/stdlib-reference.md.)
- **Test harness rewires.** example.bat EXCLUDE trimmed (7 moved-out basenames removed);
  lsp_bulk_test.py skips `ui_native_winui.cb` (unresolvable App SDK winmds in the bare
  sweep, like the winui/ launchers) and lists `ui_native_win32.cb` as Windows-only
  (imports windows.h). The other new core hosts (`ui_native.cb`/`_host`/`_cocoa`)
  analyze clean on Windows and are swept.
- **Docs.** doc/UI.md retitled to `ui_native`, all import/path examples updated to the
  core names, and the standalone `v1.0-rc`/`API v18` version scheme REMOVED (replaced
  with a note that the framework ships with + is versioned by the compiler - user
  decision, cflat is unpublished). Parity matrix + WinUI/Cocoa gaps kept.
  internal/stdlib-reference.md gained a UI section.
- **NO Test/test_ui.cb** (user decision) - example.bat stays the gate.

CAVEAT carried forward unchanged: **macOS is COMPILE-VERIFIED ONLY**
(`--check --platform macos` green for fedit/gallery/cocoa_native_settings); no arm64
box was available, so mac RUNTIME verification of the gallery/fedit self-tests is
still DEFERRED. This is the one P11 debt promotion did not close.

Gates (all green on Windows):
- `cmake_build.bat release` clean; `cflat.exe --init` exit 0 (core bitcode cache rebuilt).
- test.bat Release: all tests passed.
- test_lsp.bat: 197 passed, 0 failed (the new `ui_native*.cb` core files analyze clean).
- example.bat Release: 86 passed, 0 failed, 21 skipped (skip fell from 28: 7 files left
  example/ for core/). gallery 27/27, fedit 14/14, win32_native_settings 8/8 - all
  leak-clean under --heap-audit; winui demos pass.
- Cocoa + WinUI `--check` green for fedit/gallery/cocoa_native_settings (`--platform
  macos`) and winui_demo/winui_gallery.

Original spec (for reference):

Now mechanical: the name, the API surface, and three-host stability were
all settled in P11-P13.

- Move ui.cb + ui_native.cb + hosts into core/ with DeploymentContent
  entries (and the CMake deploy list); `import "ui.cb"` just works with no
  -i gymnastics (also kills the multiple-import-dirs pain for UI apps).
  Apply the P13-chosen name in the same change.
- Stamp doc/UI.md v1.0 (the P13 rc pass means this is a version bump, not
  an edit).
- test.bat picks up a Test/test_ui.cb smoke (headless, no window shown - or
  keep it in example.bat if window creation is too environment-sensitive;
  decide at the milestone).
- Stretch (explicitly optional, do not block v1.0): DatePicker, hyperlink
  (SysLink), font/color pickers, drag-and-drop, clipboard helpers.

### Sequencing rationale

P7 before P8 because ListView/TreeView demos and fedit v2 need multi-window
+ post(); P8 before P9 because the setListOp seam (hardest new interface
design) should be proven on ListView before TreeView/Tab reuse it; gallery
lands mid-stream (P10) so parity has a single carrier to port. For the
pre-promotion tail: Cocoa (P11) before WinUI (P12) because the Cocoa stub
debt is older and larger, and porting the gallery off raw SendMessage
asserts onto host-neutral drivers - which P12 then reuses - has to happen
for the first non-Win32 port anyway; hardening (P13) after both parity
sweeps so the freeze, the a11y/focus audit, and the naming decision happen
against the real three-host surface; promotion (P14) last and mechanical.
Compiler work expected: NONE planned - every control above rides existing
interop (proven by P1-P10, where all compiler findings were filed bugs, not
missing features); if one surfaces, file it in internal/issue/ and escalate
per the P6 M1 precedent.

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
   ListView + NSTableView + WinUI ItemsView before coding. RESOLVED for P8: the
   op-coded protocol is proven on Win32 ListView (LVS_OWNERDATA) and stubbed on
   Cocoa/WinUI; P11 validates the NSTableView/ItemsView bodies against the same ops.
4. Multi-window refactor (P7) touches the host globals every existing
   self-test relies on - migrate tests in the same change, gate on all of
   them.

## Naming

Working name "fedit" for the demo app; framework itself stays `ui.cb` until
core/ promotion (name bikeshed decided in P13, applied in P14).
