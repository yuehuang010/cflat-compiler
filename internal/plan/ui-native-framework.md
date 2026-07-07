# Plan: Native UI Framework (Electron-competitor, native OS controls)

Status: P0-P5 DONE (Windows + macOS) + P6 M1-M3 DONE (WinUI 3). Created: 2026-07-03

Phase status:
- P0: DONE 2026-07-03. example.bat Release 79/0/12 (baseline 76/3/11; the 3 were
  vcpkg lock-race flakes, green this run), test.bat Release green. Additive seams
  only, Canvas/TUI self-tests byte-identical. Added: ui_native.cb (NativeHost
  interface + PROP_*/FONT_* consts), UiContext.nativeByKey shadow map + accessors,
  Patch.key identity threaded through reconcile via reconcilePath, doc/UI.md API v9.
  Interpretation note: layout ints are reframed as DIPs (TUI reads 1 DIP == 1 cell
  so it stays byte-identical; native hosts scale DIP->px at the boundary). No layout
  algorithm change was needed, so no risk to the byte-identical gate.
- P1: DONE 2026-07-03. example.bat Release 81/0/13, test.bat Release green. Added
  win32_native_host.cb (Win32Host : NativeHost - real BUTTON/STATIC/EDIT/checkbox/
  msctls_progress32/trackbar/etched-panel; tree-walk-diff keyed by key path against
  UiContext.nativeByKey; DeferWindowPos batching; WM_COMMAND/WM_HSCROLL routing via a
  live key->handle scan; OS-owned Tab via IsDialogMessageA; focus mirror via
  BN/EN_SETFOCUS; applying guard for EN_CHANGE reentrancy) and win32_native_settings.cb
  (settings on real controls + headless state-assert self-test 7/7, wired into
  example.bat as --worker-native under --heap-audit). Added Element.nodeBounds() to
  ui.cb (interface + all 10 nodes). Deviation: native host uses tree-walk-diff
  (reconcile-to-native), not Patch-stream consumption, for control lifecycle -
  equivalent + more robust; Patch.key identity still exists. Compiler bug found +
  WORKED AROUND (filed internal/issue/iface-bind-owning-value-zeroes-source.md):
  binding an owning-value class (string field) to an interface slot zeroes the
  aliased source -> components must be heap-allocated (new). Plus a ternary-over-
  owning-field aliasing heap-corruption, worked around with "" + field.
- P2: DONE 2026-07-03. example.bat Release 81/0/13, test.bat Release green (native
  self-test extended to assert the host applied the dark theme). Added to
  win32_native_host.cb: per-monitor-v2 DPI awareness (SetProcessDpiAwarenessContext
  (-4)); DIP->px now scales by GetDpiForWindow (dev box is 192 dpi/200% - verified
  controls stack cleanly, no overlap); WM_DPICHANGED relayout + font rescale +
  move-to-suggested-rect; Segoe UI Variable Text font (fallback Segoe UI) sized to
  dpi; DWMWA_USE_IMMERSIVE_DARK_MODE dark titlebar driven by the app Theme; themed
  control/window colors via WM_CTLCOLORSTATIC/EDIT/BTN + WM_ERASEBKGND (documented
  APIs only, no uxtheme ordinals). NativeSettings now sets ctx.theme by dark toggle.
  Deferred (consistent with plan's "hover/pressed only on custom-drawn accents"):
  NM_CUSTOMDRAW owner-drawn button accent fills - native push buttons keep the OS
  themed look + OS hover/pressed animation, which we deliberately do not fight.
- P3: DONE 2026-07-03 (editor essentials). Added to ui.cb: TextArea element
  (ELEM_TEXTAREA) - multiline, UNCONTROLLED-with-sync (native EDIT is the source of
  truth; `value` is a push, `onChange` is a dirty notify; propsEqual ignores value
  so a re-render never clobbers the buffer). Added to win32_native_host.cb: TextArea
  -> multiline EDIT (ES_MULTILINE|WS_VSCROLL|ES_WANTRETURN), seeded once at create,
  never re-pushed by render; nativeControlText/nativeSetControlText (pull/push, push
  guarded so it is not a user edit); declarative menu bar (menuReset/menuAddTop/
  menuAddItem/menuAddSeparator + one setMenuHandler dispatcher) realized as HMENU +
  accelerator table (parseAccel "Ctrl+Shift+S"/"F3" -> ACCEL, TranslateAcceleratorA);
  WM_COMMAND lParam==0 routes menu/accelerators; file dialogs (nativeOpenFile/
  nativeSaveFile via comdlg32 GetOpenFileName/GetSaveFileName - real shell dialogs);
  MessageBox helpers (nativeConfirm YES/NO/CANCEL, nativeInfo); WM_CLOSE dirty-prompt
  hook (setCloseQuery); a real secondary top-level window (showSecondaryWindow, own
  class+WndProc, does not quit on close); nativeCloseWindow + nativeTeardownForTest.
  Deviations: file dialogs use comdlg32 not IFileDialog COM (visually identical, more
  robust; COM version deferred); ListView + ctx.post() thread marshaling NOT built
  (fedit does not need them - deferred); "two windows" = main editor + About window
  (full multi-editor-window host refactor deferred to keep the single-window globals
  and P1/P2 gates intact). The ternary-over-owning-string aliasing hazard recurred in
  parseAccel + fedit status; worked around with plain if-assignments.
- P4: DONE 2026-07-03 (flagship). Added example/ui/fedit/fedit.cb - a real native
  editor: open/edit/save/save-as (shell dialogs), find bar (Ctrl+F / F3), dirty
  marker + dirty-close prompt, light/dark switch (Ctrl+T, immersive dark titlebar),
  Help>About second window, real menu bar + accelerators, multiline native editor.
  Headless state-assert self-test 6/6 (open, edit->dirty via EN_CHANGE, save
  round-trip, menu routing, theme, find-next), leak-clean, wired into example.bat as
  --worker-fedit under --heap-audit. Visually confirmed via PrintWindow.
- P5: DONE 2026-07-05 (macOS AppKit host, commits ad01bf8 + 50b4807 + POSIX
  fixes in e5058ef). The objc spike ran FIRST as planned: example/macos/cocoa.cb
  is a dlopen/objc_msgSend bridge (no SDK, no Xcode, no -framework linking) and
  cocoa_probe.cb --selftest validated the per-signature msgSend fn-ptr casts and
  the NSRect 4-double HFA return (d0-d3) against the cflat function<> ABI - the
  plan's front-loaded ABI risk resolved with NO compiler prerequisite. Added
  example/ui/cocoa_native_host.cb (CocoaHost : NativeHost - real NSButton/
  NSTextField/NSSlider/NSProgressIndicator/NSBox + TextArea as NSTextView;
  FLIPPED content view so coordinates stay top-left like Win32; DIP->points via
  fixed BASE_X=8/BASE_Y=26 with NO DPI machinery since Cocoa points are already
  density-independent; declarative NSMenu menu bar with accelerators - Ctrl maps
  to Cmd per mac convention, item routing via setTag; same public surface as the
  Win32 host) and example/ui/native_host.cb, an if const (__MACOS__) import shim
  so one app source compiles against either backend. cocoa_native_settings.cb
  ports the settings self-test; fedit runs on macOS unchanged through the shim
  (50b4807 fixed a missing text-width in the host). Gate: example_mac.sh (mac
  counterpart of example.bat - cocoa_probe/cocoa_window/cocoa_settings/fedit
  headless self-tests; HeapAudit is Windows-only so the leak gate is the shared
  nativeTeardownForTest path) plus test.sh/test_lsp.sh green on the arm64 box.
Predecessor: ui-framework-v5-sugar-widgets.md (lost with the old internal/plan/;
the shipped result is example/ui/ at API v8 - see doc/UI.md and the recap in
project memory).

## Locked decisions (user, 2026-07-03)

1. Windows tier: **Win32 common controls now, WinUI-3-ready host interface**.
   The NativeHost seam must be opaque-handle + property-based so a WinUI 3
   backend can slot in later once WinMD delegate projection lands. No Windows
   App SDK dependency in v1 (self-contained exe stays the anti-Electron pitch).
2. Flagship v1 anchor: **a small real tool - text editor/viewer** ("fedit").
   Menus, file dialogs, keyboard shortcuts, multiline text view, status bar,
   at least two windows. This forces the framework past forms-demo scope.
3. Look policy: **native controls + styled accents**. Controls are real OS
   widgets; our Theme colors are honored via NM_CUSTOMDRAW / WM_CTLCOLOR* on
   Windows and NSAppearance/bezelColor/contentTintColor on macOS. Native
   accessibility, IME, and text rendering come for free and are a headline
   differentiator vs Electron.

## Thesis

Keep the app-facing model exactly as shipped in example/ui/ui.cb: retained
Element tree, Component, controlled widgets, keyed identity, Theme,
`<View/>` sugar. Replace the *output* stage: instead of painting through
Canvas, the reconciler's patch stream drives create/update/destroy of real
native controls (HWND / NSView), our layout engine computes pixel frames,
and the OS paints. This is the React Native architecture, minus the JS
bridge - CFlat compiles to native, so "the bridge" is a direct call.

The Canvas backends do NOT die: SurfaceCanvas (TUI) and GdiCanvas
(win32_shot/self-tests) remain as the headless/testing/embedded path.
Element stays renderer-agnostic.

## Architecture

```
App render() -> Element tree -> reconcile (existing) -> Patch stream
                                                          |
                    +-------------------------------------+
                    v                                     v
              Canvas hosts (existing)              NativeHost (new)
              SurfaceCanvas / GdiCanvas       Win32Host | AppKitHost | (WinUI3Host later)
```

### 1. NativeHost interface (new, in ui.cb or ui_native.cb)

Opaque `u64` handles (HWND on Windows, NSView* on macOS). Property-style
setters, no per-toolkit types in the interface:

```cflat
interface NativeHost
{
    u64  createControl(int elemKind, u64 parentHandle, string key);
    void destroyControl(u64 h);
    void setFrame(u64 h, Rect frameDip);
    void setText(u64 h, const char* s);
    void setBoolProp(u64 h, int prop, bool v);     // checked, enabled, visible
    void setIntProp(u64 h, int prop, int v);       // value, max, caret, scrollY
    void setAccent(u64 h, int fgColor, int bgColor); // 0 = native default
    Size measureText(const char* s, int fontId, int wrapWidthDip);
    void requestLayout();                          // schedule relayout+apply
    u64  postToUiThread(/* see threading */);
}
```

Design rules that keep it WinUI-3-ready:
- Handles are opaque and never dereferenced by ui.cb.
- All mutations are property sets keyed by (handle, prop) - maps 1:1 to XAML
  DependencyProperty sets later.
- Events flow back only through the existing `Event` struct + `dispatch()`
  seam; the host owns the translation.

### 2. Patch stream gains identity (prerequisite compiler-free change)

Today `Patch { op, nodeKind, detail }` is debug-grade: no node identity, so a
host cannot apply it. Extend to carry the stable key path (parent chain of
`keyOf()` / synthesized position keys) plus the affected Element pointer for
UPDATE prop diffs. Reconcile already matches by key (`indexByKey`); the work
is threading the path through and auto-assigning positional keys to unkeyed
children at mount (so every live node has a stable identity). UiContext gains
`dictionary<string, u64> nativeByKey` - the shadow map from element identity
to native handle.

Commit protocol per frame: reconcile -> patch list -> host applies in one
batch (BeginDeferWindowPos/EndDeferWindowPos on Win32; single CATransaction
on AppKit) -> layout pass -> setFrame batch. Batching is the flicker defense.

### 3. Layout: cells -> DIPs

- `Rect`/`LayoutConstraints`/`Size` move to int DIPs (device-independent
  pixels, 96-dpi logical). TUI keeps cells: SurfaceCanvas hosts pass a
  cellSize and ui.cb divides - the layout algorithm itself is unit-blind, so
  this is a units-and-constants change, not an algorithm change.
- Text measurement becomes a host callback (`measureText`), replacing the
  1-cell-per-char assumption. GDI: DrawText DT_CALCRECT; AppKit: NSString
  sizeWithAttributes. TUI host: strlen (unchanged behavior).
- DPI: Win32 per-monitor-v2 manifest/SetProcessDpiAwarenessContext,
  GetDpiForWindow scales DIP->px at the host boundary only; WM_DPICHANGED
  triggers relayout. macOS: AppKit points are already DIPs; backing scale is
  the OS's problem.
- Widget intrinsic sizes come from OS metrics (button padding, edit height)
  via a host `defaultMetrics(elemKind)` query, not hardcoded cell counts.

### 4. Events and focus

- Native event -> `Event` -> existing `dispatch(Event, ctx)` - unchanged seam.
- Win32: one shared subclass proc (SetWindowSubclass) on every created
  control + WM_COMMAND/WM_NOTIFY on the parent; handle->key reverse map
  routes to the element.
- Focus INVERTS: the OS owns real focus (Tab order, screen readers). Our
  keyed focus in UiContext becomes a mirror: WM_SETFOCUS/NSResponder
  notifications update `ctx.focusKey`; `ctx.requestFocus(key)` calls
  SetFocus/makeFirstResponder. collectFocusables/moveFocus become the
  fallback for Canvas hosts only.
- Controlled-widget reentrancy: EN_CHANGE fires when *we* SetWindowText. Use
  an `applyingPatches` guard flag in the host to swallow self-inflicted
  notifications - decide this on day one, it is the classic native-React bug.

### 5. Text editing (the editor anchor's core widget)

New `TextArea` element: multiline, scrollable text view.
- Win32: EDIT with ES_MULTILINE (RichEdit only if we need styling later).
- macOS: NSTextView in NSScrollView.
- NOT controlled per-keystroke (round-tripping a 1MB buffer per key is the
  Electron-class mistake). Model: native buffer is the source of truth;
  element exposes `onChange(dirty)` notification, `getText()` (pull, owned
  string), `setText()` (push, e.g. file open). This is a deliberate,
  documented exception to the controlled-widget rule for large-buffer
  widgets.

### 6. Windowing, menus, dialogs (editor essentials)

- `Window` class: owns a native top-level + a root Element + its UiContext.
  App = `list<Window>`; one UI thread, one message loop for all windows.
- Native menu bar: declarative `Menu`/`MenuItem` tree on Window (label,
  accelerator string "Ctrl+S", enabled, onSelect closure). Win32
  HMENU/AppendMenu + WM_COMMAND; macOS NSMenu (mandatory for a credible Mac
  app - the menu bar IS the app on macOS).
- Accelerators: parse "Ctrl+S" once into an accel table
  (ACCEL/TranslateAccelerator; keyEquivalent on NSMenuItem).
- Dialogs: `openFileDialog/saveFileDialog/messageBox` as blocking host calls
  (IFileOpenDialog/IFileSaveDialog via COM vtable interop - proven pattern
  from the DirectX work; NSOpenPanel/NSSavePanel/NSAlert).

### 7. Styled accents (look policy)

- Theme struct survives as-is; `0` still means "native default" (the
  rgb()-marker design already handles this).
- Win32: DWMWA_USE_IMMERSIVE_DARK_MODE titlebar; WM_CTLCOLORSTATIC/EDIT/BTN
  for fg/bg; NM_CUSTOMDRAW for button accent fills + focus ring;
  Segoe UI Variable (fallback Segoe UI) via SystemParametersInfo metrics.
  Explicitly documented-API-only - no uxtheme ordinal hacks.
- macOS: NSAppearance (aqua/darkAqua) per window; bezelColor/
  contentTintColor; layer backgroundColor for View/Box fills.
- shade()/hover/pressed logic stays in ui.cb where the host does custom-draw;
  where the OS already animates (macOS buttons), we do NOT fight it - accents
  set colors, never re-implement OS interaction feedback.

### 8. Threading

UI-thread affinity enforced: `ctx.assertUiThread()` in debug builds. Cross-
thread work posts via host (`PostMessage(WM_APP_CALL)` /
performSelectorOnMainThread) wrapping a `function<void()>` - closures are
clone-by-default owning values, which is exactly right for handoff. This is
required for the editor (async file IO off-thread).

### 9. macOS backend: AppKit via objc runtime

- New library (example/ui/objc.cb until promotion): externs for
  objc_getClass, sel_registerName, objc_msgSend, objc_allocateClassPair,
  class_addMethod, objc_registerClassPair.
- ABI RISK (front-load a spike): on arm64 Darwin, variadic calls put anon
  args on the stack, but objc_msgSend must be called with the *exact fixed*
  signature of the target method. A single variadic extern is WRONG. Plan:
  take &objc_msgSend and cast through per-signature function-pointer types
  (`id(*)(id,SEL)`, `id(*)(id,SEL,id)`, ...). Spike test: create NSString,
  call length, before building anything on it. If fn-ptr signature casts hit
  a compiler gap, that becomes a prerequisite compiler work item.
- Delegates/target-action: build ONE bridge class at runtime
  (objc_allocateClassPair) whose IMP is a cflat function; instance ivar holds
  the element key. No compiler work needed.
- Struct-return msgSend (NSRect) is fine on arm64 (no objc_msgSend_stret).

### 10. WinUI 3 later (explicit non-goal for v1)

Blocked on WinMD delegate projection (compiler). When it lands: WinUI3Host
implements NativeHost; createControl -> ActivateInstance of
Microsoft.UI.Xaml.Controls types; setFrame -> Canvas.SetLeft/Top or a raw
panel; events via projected delegates. Nothing in v1 may leak Win32 types
above the host boundary - that is the whole "WinUI-ready" contract.

## Phasing

- **P0 - seams (no visible change):** Patch identity + key paths + shadow-map
  plumbing; DIP-ify layout with cell adapter for TUI; measureText host
  callback; NativeHost interface committed to doc/UI.md (API v9). Gate: all
  existing Canvas self-tests byte-identical (cells==DIPs for TUI host).
- **P1 - Win32Host MVP:** window + Button/Text(STATIC)/TextInput(EDIT)/
  Checkbox(BM_*)/ProgressBar(msctls_progress32)/Slider(trackbar)/Box; patch
  apply + DeferWindowPos batching; events + focus mirror; win32_settings.cb
  runs on real controls. Gate: headless self-test = create invisible window,
  drive with SendMessage, assert control state via GetWindowText/BM_GETCHECK
  (no pixel asserts needed - the OS paints).
- **P2 - modern look:** DPI awareness + WM_DPICHANGED, dark titlebar, theme
  accents via WM_CTLCOLOR/NM_CUSTOMDRAW, Segoe UI Variable, hover/pressed on
  custom-drawn accents only.
- **P3 - editor essentials:** Window class + multi-window loop; Menu/
  accelerators; file dialogs (IFileDialog COM); TextArea (multiline EDIT,
  uncontrolled-with-sync); ListView-backed list (for an open-files pane);
  StatusBar; thread marshaling (ctx.post).
- **P4 - flagship:** example/ui/fedit/ - open/edit/save text files, find bar,
  dirty-close prompt, two windows, light/dark. This is the demo that makes
  the Electron comparison.
- **P5 - macOS AppKit host:** objc.cb spike FIRST (msgSend fn-ptr casts),
  then AppKitHost to parity with P1-P3; port fedit; NSMenu done properly.
  Builds on the working arm64 toolchain (test.sh green).
- **P6 - WinUI 3 track (M1-M3 DONE 2026-07-03):**
  - **M1 DONE 2026-07-03 - WinMD delegate projection (compiler).** New builtin
    `winrtDelegate(DelegateType, closure)` synthesizes a refcounted COM-callable
    object (vtable {QueryInterface, AddRef, Release, Invoke}) wrapping a cflat
    closure. Metadata-driven (works for ANY winmd delegate): IID is the stored
    delegate IID (non-generic) or the derived PIID (parameterized, same Rogers
    pinterface derivation as interfaces). Invoke marshals the WinRT ABI args
    through the existing thin-slot mapping and calls the closure env-last; the
    closure is cloned in (clone-by-default) and destructed on final Release.
    Files: CFlat.g4 (WinrtDelegate token + primary alt), WinmdExtract.cpp
    (delegate genericParams now read), LLVMBackend.h (BuildWinrtDelegateType +
    EmitWinrtDelegateObject + WinrtDelegateInvokeParamTypes + FindWinrtDelegate),
    MainListener.h (primary-expr handler). Proof: Test/test_winmd.cb gains
    testDelegateSyncInvoke (deterministic: AddRef/Release refcount + capturing
    closure Invoke) and testDelegateAsyncCompleted (REAL WinRT
    AsyncOperationCompletedHandler<String> via put_Completed - Windows QIs the
    derived PIID and calls our Invoke on a threadpool thread). Gates green:
    test.bat Release all pass (test_winmd 10/10), test_lsp 187/0, example.bat
    82/0/14. Lifetime: object holds an owned clone of the fat closure {code,env};
    Release atomically decrements, runs __closure_fat_ptr.dtor on the closure
    field (frees the heap env), then operator delete. asan-clean.
  - **M2 DONE 2026-07-03 - real WinUI 3 window headless from an unpackaged exe.**
    example/ui/winui/winui_app_demo.cb brings up a full XAML Application + Window +
    Button, wires Click via winrtDelegate(RoutedEventHandler), and verifies the handler
    fires by programmatically invoking the button through its UI Automation peer from a
    DispatcherQueue-posted callback, then Application.Exit(). Self-test PASS, exit 0,
    unattended (`winui_app_demo.exe --selftest`). KEY FINDING: **no IApplicationOverrides
    aggregation/composition is needed.** Application.Start(callback) runs its own message
    pump; inside the callback we create a *base* Application via
    `IApplicationFactory::CreateInstance(nullptr outer, &inner, &app)` (null-outer
    composition sets Application.Current) and build the Window directly - OnLaunched
    override is only needed if you rely on launch args, which we do not. This sidesteps
    the hardest anticipated piece entirely. Chain: MddBootstrapInitialize2 -> RoInitialize(0)
    STA -> IApplicationStatics::Start(winrtDelegate ApplicationInitializationCallback) ->
    (in callback) base Application + Window (IWindowFactory null-outer) + Button
    (RoActivateInstance) + boxed-string content (PropertyValue.CreateString +
    IContentControl::put_Content) + add_Click -> put_Content(window) + Activate -> post a
    DispatcherQueueHandler that CreatePeerForElement + GetPattern(Invoke=0) + QI
    IInvokeProvider + Invoke() (the returned pattern object MUST be QI'd to IInvokeProvider
    - calling the provider vtable on the raw peer-returned object crashes) -> Exit. Bootstrap
    lib/dll from the NuGet cache (link .lib via --c-lib, copy .dll next to exe); winmds
    Microsoft.UI.Xaml.winmd + Microsoft.UI.winmd (DispatcherQueue is in the
    Microsoft.UI.Dispatching namespace inside Microsoft.UI.winmd) live in
    example/ui/winui/winmd/ (-i). NO compiler change needed for M2.
  - **M3 DONE 2026-07-03 - WinUI3Host behind the NativeHost seam.** example/ui/winui/
    winui_host.cb implements `WinUI3Host : NativeHost` (same contract as
    win32_native_host.cb): createControl -> RoActivateInstance of Button/TextBlock/CheckBox/
    Border rooted in a Canvas (Canvas.Children as IVector<UIElement>::Append - PIID derivation
    for the runtimeclass type-arg WORKS); setFrame -> Canvas.SetLeft/SetTop + Width/Height
    (DIP passthrough, near-1:1); setText -> ITextBlock::put_Text or IContentControl::put_Content
    (boxed); setBoolProp -> IToggleButton::put_IsChecked (boxed IReference<bool>) /
    IControl::put_IsEnabled / IUIElement::put_Visibility; events (Click, Checked/Unchecked)
    wired via winrtDelegate into the existing fireClick/fireToggle + reconcile seam.
    winui_demo.cb runs a real Component (checkbox enables a button; clicks bump a counter shown
    in a TextBlock) through the SAME ui.cb reconcile machinery - nothing above the NativeHost
    seam changed. Headless self-test 3/3 (create, checkbox->enable via UIA Toggle, click->count
    via UIA Invoke, TextBlock readback), exit 0, unattended. The event closures capture the
    control HANDLE (a u64) and reverse-map to the node key via keyForHandle at event time - the
    same handle->key routing win32_native_host.cb uses. TWO compiler changes made during this
    track: (1) winmd value-struct registration is now first-writer-wins (LLVMBackend.h
    RegisterWinrtModel Pass B) so Windows.Foundation.Rect no longer clobbers a program's own
    `struct Rect` (ui.cb's Rect has int x/y/w/h; the WinRT Rect has float X/Y/Width/Height) -
    without this, importing the winmds broke ui.cb's own layout. (2) closure captures of an owning
    `string` are now DEEP-COPIED into the heap env, which owns the copy via a per-closure cleanup fn
    stored in the env header (MainListener closure emit + GenerateClosureCaptureCleanup +
    __closure_fat_ptr.copy/.dtor + function.cb env primitives; the unpacked capture local is marked
    IsAliasBorrow so the env frees it exactly once) - previously a captured owning string was a
    shallow alias of the source and read empty once the source was destructed (the bug that forced
    the handle-capture routing). Scoped to `string` (its deep copy is correct); other owning value
    types keep prior capture behavior. test_winmd stays green and gains testDelegateStringCapture;
    no user/winmd struct name collision exists in the suite.
  - **P6 gates (2026-07-03):** test.bat Release all pass (test_winmd green), test_lsp 188/0
    (winui/* excluded from the bulk sweep - they need the winmd -i dir + App SDK), example.bat
    84/0/17 with both winui self-tests PASS. The winui self-tests are gated WITHOUT --heap-audit
    (documented in example.bat): Application/Window/DispatcherQueue are process-lived singletons
    never torn down before ExitProcess, and the WinAppSDK allocates through its own heaps, so a
    leak oracle over cflat's audited allocator does not apply. Deferred within M3 (same pattern,
    not built): TextInput (TextBox), Slider/ProgressBar (IRangeBase), theme accents, multi-window.
- **P6+ (deferred):** WinUI3Host (needs delegate projection - now landed), core/
  promotion of ui.cb, GPU-composited custom Canvas widget inside a native
  host view (the "app picks per widget" escape hatch), virtualized lists.

## Testing strategy

- example.bat stays the gate, all UI self-tests under --heap-audit.
- Canvas hosts keep the pixel-level asserts (guiSelfTest et al) - they now
  primarily guard layout + reconcile, which are shared with native hosts.
- Native host tests are STATE asserts on invisible windows (Win32) /
  non-activated NSApplication (macOS): create, patch, SendMessage/action,
  read back control state. Interactive-input probes can reuse the
  WriteConsoleInput-style injection philosophy (never "verified by hand").
- win32_shot.cb-style screenshots: add a native variant using PrintWindow
  into a DIB for occasional visual review.

## Risks / open items

1. objc_msgSend signature-cast support in cflat fn pointers - SPIKE in P5
   opener; potential compiler prerequisite.
2. EN_CHANGE/notification reentrancy - guard flag designed in from P1.
3. Flicker on reconcile bursts - DeferWindowPos batching + WS_CLIPCHILDREN;
   fall back to WS_EX_COMPOSITED only if needed.
4. Win32 dark mode for *control interiors* is officially unsupported - we
   cover it via WM_CTLCOLOR + custom-draw accents (documented APIs only);
   accept stock scrollbars staying light in v1.
5. Patch/identity refactor touches reconcile - the trickiest shared code;
   P0 gates on existing tests byte-identical before any host exists.
6. Menu/dialog closures held by native structures - lifetime audit under
   --heap-audit (closures are owning values; menu teardown must destruct).

## Naming

Working name "fedit" for the demo app; framework itself stays `ui.cb` until
core/ promotion (name bikeshed deferred to promotion time).
