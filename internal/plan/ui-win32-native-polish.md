# Win32 native host: scrolling, paint correctness, dark theming

Created: 2026-07-13 (gallery ran on Win32 for the first time; this is the fallout)
Status: Phases 1-4 DONE and committed (2800f22). Phase 5 (embed the comctl32 v6 manifest)
NOT STARTED. The linker mechanics are VERIFIED (5.1); how the manifest content is DECLARED
is still in brainstorm (5.1a) - the user does not want it hardcoded in the compiler.

## Context

`example/ui/05-gallery` was brought up on the Win32 host and immediately showed a
cluster of defects. They were NOT regressions - the gallery had never run on
Windows before. This file tracks what was fixed and what is left.

All work is in `cflat/core/ui_native/win32.cb` unless noted. Gates for every
phase: `gallery.exe --selftest` (39/39 + 3 hardening cases), `test.bat Release`,
`test_lsp.bat`, `example.bat`.

## Phase 1 - scrolling was a repaint storm (DONE)

Symptom: heavy overdraw / smeared text while scrolling.

Root cause: `scrollTo` re-positioned all ~60 child HWNDs per wheel notch with
`SWP_NOCOPYBITS`, which discards each control's pixels. One notch became ~60
asynchronous `WM_PAINT`s plus a parent erase in every vacated region, so the
repaint was visible in flight. (The `SWP_NOCOPYBITS` had been added to suppress
a status-bar ghost, which was really a missing-`WS_CLIPSIBLINGS` bug.)

Fix: one `CFlatContentPane` child window (`WS_CLIPCHILDREN | WS_CLIPSIBLINGS`,
`WS_EX_CONTROLPARENT`) sized to the FULL content height parents all scrollable
content. Scrolling is one `SetWindowPos` of that pane to `y = -scrollPx`: the OS
blits the still-visible band and invalidates only the newly exposed strip. The
controls never move and never repaint - O(1) per notch instead of O(60).

Load-bearing details:
- The status bar stays a FRAME child and must be raised to `HWND_TOP` explicitly;
  `msctls_statusbar32` self-docks to the BOTTOM of the z-order and would end up
  under the content-tall pane.
- The pane answers `WM_NCHITTEST` with `HTTRANSPARENT` so a press on its bare
  background still reaches the frame (the SplitView divider hit-test lives there).
  Its children still win the hit-test.
- Content height is capped at `PANE_MAX_H` (32000): mouse messages pack client
  coords as SIGNED 16-bit, so a taller pane would corrupt hit-testing.

## Phase 2 - container controls painted over their contents (DONE)

Symptoms: GroupBox rendered as horizontal stripe garbage; TabControl content
vanished after scrolling away and back.

Root cause: controls were created without `WS_CLIPSIBLINGS`, and a GroupBox /
TabControl / Box is a CONTAINER whose contents are FLAT SIBLINGS drawn on top of
it. A repainting container painted over content that - now that a scroll only
invalidates a thin band - never repainted again, so the damage was permanent.

Fix: `WS_CLIPSIBLINGS` on every control; a z-order "sink" pass (`_sinkFrame`,
`ctFrame`, `_containerControl`) that pushes container controls BELOW their
sibling contents, mirroring the existing backdrop sink; and a WNDPROC subclass
(`ContainerCtlProc`) so GroupBox/Box erase their own interior - a themed
`BS_GROUPBOX` draws only its frame and assumes a parent background that a
`WS_CLIPCHILDREN` pane cannot supply.

## Phase 3 - right-click never showed a context menu (DONE)

Root cause: the menu is registered on a Box, which is a `STATIC` with no
`SS_NOTIFY` - it answers `HTTRANSPARENT`. The right-click falls through it (and
through the pane) onto the FRAME, so `WM_CONTEXTMENU` arrived carrying the
FRAME's handle and `keyForHandle` resolved nothing.

Fix: when `wParam` is not a control with a registered menu, hit-test the screen
point against the element tree (`showContextMenuAt` / `_ctxScan`), mapping through
the pane so the scroll offset comes along for free.

## Phase 4 - dark theming + the resize grabber (DONE)

THE KEY FINDING: **the exe carries no manifest at all**, so every cflat GUI app
runs the CLASSIC (v5) common controls. `SetWindowTheme` is therefore a no-op on
control CLIENT areas. Only three things work without a manifest: each control's
own color messages, owner-draw / custom-draw, and NON-client theming. Every fix
below rides those.

- ListView: `LVM_SETBKCOLOR` / `LVM_SETTEXTBKCOLOR` / `LVM_SETTEXTCOLOR`; the
  header has no color message, so `ListCtlProc` subclasses the ListView and draws
  the header via its `NM_CUSTOMDRAW`.
- TreeView: `TVM_SETBKCOLOR` / `TVM_SETTEXTCOLOR` / `TVM_SETLINECOLOR`.
- TabControl: no uxtheme dark theme and no `NM_CUSTOMDRAW` support ->
  `TCS_OWNERDRAWFIXED` + `WM_DRAWITEM` for the items, `ContainerCtlProc` for the body.
- ProgressBar: drop visual styles, then `PBM_SETBKCOLOR` / `PBM_SETBARCOLOR`.
- Status bar: `SB_SETBKCOLOR` + owner-drawn text. Theming now also runs at control
  CREATION, not only on a theme change (that hole left controls born outside a
  theme-change frame stock-colored).
- Resize grabber: the frame's `WS_VSCROLL` scrollbar is non-client and spans the
  full client height, so Windows painted a light classic size box in the corner.
  Replaced by a `CFlatScrollGutter` child (a 0-client-width window whose NON-client
  scrollbar IS themeable) that stops above the status bar; the status bar carries
  `SBARS_SIZEGRIP` and owns the grip.

Re-theming happens only on a real theme transition, never per pump (per-pump churn
flickers). A light/monochrome app is never touched.

## Phase 4b - Box/GroupBox content ran through the frame (DONE, host-neutral)

In `cflat/core/ui_native.cb` (NOT win32-only): `Box::layout` / `GroupBox::layout`
inset children by `1 + padding` rows on BOTH sides, but a `style.height` too small
for that left the last child overflowing the bottom border - and nothing clips it,
since on a native host the child is a sibling window drawn ABOVE the frame. The
gallery's context box (height 2, inset 1) put its label straight through the border.
Both now grow to hold their inset children instead of overflowing.

## Phase 5 - NOT STARTED: embed the comctl32 v6 manifest

Push BUTTONS, the COMBO box, the TRACKBAR thumb, and the size-grip GLYPH are still
light in dark theme. Same root cause as Phase 4: the exe has no manifest, so the app
binds the CLASSIC (v5) common controls (see
`internal/issue/win32-classic-common-controls-v5.md`).

DECISION (2026-07-13): embed a comctl32 v6 manifest, rather than owner-drawing the
remaining controls on v5. It modernizes every control at once, makes `SetWindowTheme`
genuinely work on control client areas, and lets a good deal of the Phase-4 owner-draw
be deleted instead of grown. The cost is that it changes the look of EVERY cflat GUI
app and some Phase-4 owner-draw must be re-tuned or removed - that is the work below.

### 5.1 Emit the manifest (compiler change)

The Windows COFF link args are built in `LLVMBackend.h`, in `EmitExecutable`'s
`linkArgStrs` vector (the one that already carries `/out:`, `/subsystem:console`,
and conditionally `/DEBUG` + `/PDB:`). The linker is `lld-link.exe`, located by
`DiscoverLinkerPaths` in `LLVMBackend.cpp`. The minimal form is:

    /manifest:embed
    /manifestdependency:"type='win32' name='Microsoft.Windows.Common-Controls'
                         version='6.0.0.0' processorArchitecture='amd64'
                         publicKeyToken='6595b64144ccf1df' language='*'"

VERIFIED 2026-07-13 against the deployed `x64/Release/lld-link.exe` (stub .obj, no-CRT
link; artifacts were in `scratch/manifest/`):

- `/manifest:embed` + `/manifestdependency:` links clean and the manifest lands as a real
  `RT_MANIFEST` resource (type 24, ID 1; confirmed via `FindResource` on the emitted exe).
  lld also synthesizes a `requestedExecutionLevel asInvoker` block on its own.
- **No `mt.exe` dependency.** Relinking with `PATH` cut down to `C:\Windows\System32` (so
  `mt.exe` was unreachable) still produced the embedded manifest. The Phase-5 open question
  "would this be a new toolchain requirement" is ANSWERED: it is not.
- **`/manifestinput:` works too** - our LLVM build has the manifest merger enabled. That
  matters: it means cflat can emit ARBITRARY manifest content (dpiAwareness, longPathAware,
  supportedOS, ...), not just the dependency + UAC bits lld can synthesize from flags, and
  lld will MERGE several fragments. Verified by embedding a hand-written manifest carrying
  `<dpiAwareness>PerMonitorV2</dpiAwareness>` and finding it in the exe.

Notes / things to check when doing it:

- `processorArchitecture` must match the target (`amd64` for x64). If/when another
  Windows arch is targeted this has to follow the target, not be hardcoded blindly.
- This must apply ONLY to the Windows/COFF path. `EmitExecutableElf` and
  `EmitExecutableMachO` are untouched.
- Consider whether it should be unconditional or behind a flag. Unconditional is the
  intent (every cflat GUI app should get modern controls), but a console-only program
  also gets the manifest - harmless, just note it. Do NOT add a flag "just in case"
  unless a real need appears.
- Confirm the manifest actually landed: the string
  `Microsoft.Windows.Common-Controls` must appear in the emitted exe (the check in the
  issue file), and the controls must visibly change appearance.

### 5.1a Where does the manifest CONTENT come from? (BRAINSTORM - NOT DECIDED)

Hardcoding the common-controls dependency in `EmitExecutable` would ship Phase 5 in an
hour, but it bakes a Windows vocabulary item into the compiler - the same mistake as
special-casing `windows.h` in C interop. The direction being explored instead: the manifest
is declared in `.cb` SOURCE, the compiler is only a transliterator, and the vocabulary lives
in `core/`.

Constraints stated by the user:

- NO XML in `.cb`. The surface is either a JSON-ish literal or a TYPE DECLARATION.
- Compile-time validation, WITHOUT the compiler hardcoding the manifest vocabulary.
- No separate `.manifest` file in the project (a compiler-internal temp for
  `/manifestinput:` is fine - the user never sees it).

Findings that constrain the design:

- **There is no manifest type in any header or WinMD to bind to.** Searched the Windows SDK:
  the only `.xsd` files under `Include` are ETW/event/counter manifests; the App Cert Kit's
  `ManifestValidationRules.xsd` is appx-cert rules, not the SxS assembly schema. Headers
  mention `assemblyIdentity` / `dpiAwareness` / `RT_MANIFEST` only as a resource-type
  constant and in comments. Microsoft never published the application-manifest schema as C
  structs or WinRT types. So the "back it to objects from headers/winmd" idea has NO source
  of truth; the type must be authored, in `core/*.cb` (stdlib data, not compiler C++).

- **`CreateActCtxW` is a free, always-current validator, callable at compile time.** It is
  literally what the loader runs at process start, so a manifest that passes cannot fail at
  launch with error 14001 ("side-by-side configuration is incorrect") - the normal manifest
  failure mode, usually discovered by the end user. Probed 2026-07-13:

  | manifest defect                                | CreateActCtxW |
  |------------------------------------------------|---------------|
  | malformed XML / unbalanced nesting              | REJECTS (14001) |
  | typo'd `publicKeyToken`                         | REJECTS (14001) |
  | `version` that does not exist in WinSxS         | REJECTS (14001) |
  | bogus `<windowsSettings>` value (`PerMonitorV9`)| ACCEPTS - not validated |

  It needs only the XML, NOT a linked exe, so it can run in the `--check`/analysis path and
  surface as a live LSP squiggle - not just at link time. It is Windows-host-only; a Linux
  host cross-compiling to Windows degrades to well-formedness only.

- **Type declaration beats JSON on the LSP criterion.** `LspSymbolIndex` is built during
  compilation and indexes struct fields as `Type.field` / `SymbolKind::Field` - that is what
  already powers hover, go-to-def and prefix completion. A manifest declared as a struct type
  in `core/manifest.cb` therefore gets editor typing nearly for free; a JSON literal has no
  type and gets nothing. Named brace-init already parses (`fieldInit : Identifier '='
  assignmentExpression`, CFlat.g4:527), so `{ dpiAware = ..., dependencies = { ... } }` is
  valid syntax today.
  GAP: completion does not currently fire INSIDE a brace-init - `LspServer.cpp` triggers
  member completion only after `.` / `->` (line ~229). But it already maintains a brace stack
  carrying the enclosing type per open brace (line ~293, built for implicit `this`), so
  "cursor inside a brace-init of a known struct type -> complete its fields" is a contained
  addition that would pay off for every brace-init in the language, not only manifests.

Sketch of the layering (to be argued, not yet agreed):

- COMPILER knows only XML SHAPE, never Windows vocabulary. Structural mapping: nested struct
  -> child element, scalar field -> attribute. That cannot express `<windowsSettings>`
  (element-with-text plus an `xmlns`), so the likely escape is two generic marker types
  declared in core (`attr<T>` / `elem<T>`) - the compiler then knows two XML primitives and
  still never knows what `dpiAwareness` is.
- CORE owns the vocabulary and the magic constants (nobody should ever hand-type
  `6595b64144ccf1df`). If manifest fragments compose, `core/ui_native/win32.cb` can declare
  the v6 dependency itself, so `import "ui_native.cb"` drags in its own OS requirement - the
  same ownership shape as the existing `lib` and `pri` clauses. lld merges fragments via
  multiple `/manifestinput:`.
- WINDOWS is the semantic backstop via `CreateActCtxW`; the type checker is the editor-time
  layer.

Open questions for the next round:

- What actually triggers the compiler to treat a global as the manifest? (A `manifest`
  keyword on a declaration is the smallest grammar change; it adds one keyword and zero
  vocabulary.)
- Const-eval: the initializer is literals + enum refs + nested braces, so the compiler can
  fold it straight off the parse tree - no general const-eval needed (const globals are a
  known unimplemented gap; do NOT make this depend on them).
- Do user `.cb` files get to declare manifests at all in v1, or is it core-only until the
  shape proves itself?
- Conflict rules when two fragments disagree (two different Common-Controls versions).
- The `attr<T>` / `elem<T>` marker types: acceptable, or is there a mapping that needs no
  compiler-side XML primitives at all?

### 5.2 Init the common controls properly

`win32.cb` currently calls `InitCommonControls()`. With v6 present, switch to
`InitCommonControlsEx` with an `INITCOMMONCONTROLSEX` carrying the classes actually
used (`ICC_WIN95_CLASSES` covers listview/treeview/tab/trackbar/progress/statusbar;
add `ICC_STANDARD_CLASSES`). Without this, v6 control classes may fail to register
and `CreateWindowExA` on them returns null.

### 5.3 Re-tune / delete the Phase-4 owner-draw

With v6 + `SetWindowTheme(ctl, "DarkMode_Explorer")` actually working, revisit each
Phase-4 workaround and prefer deleting it over keeping it:

- ProgressBar: the `SetWindowTheme(pb, "", "")` + `PBM_SETBKCOLOR`/`PBM_SETBARCOLOR`
  route works precisely BECAUSE the control is unthemed. Under v6 a themed progress bar
  IGNORES those colors. Expect to switch to the theme + a dark-mode look, or keep it
  deliberately unthemed. This one WILL change behaviour - check it first.
- TabControl: `TCS_OWNERDRAWFIXED` + `_drawTabItem` + the `ContainerCtlProc` body fill
  may be replaceable. NOTE: even under v6, `SysTabControl32` has no dark theme, so this
  one probably STAYS. Verify rather than assume.
- ListView header custom-draw (`ListCtlProc`): under v6 the header is themeable
  (`DarkMode_ItemsView` / `ItemsView`); the custom-draw may become unnecessary.
- Status bar: `SetWindowTheme(sb, "", "")` + `SB_SETBKCOLOR` + owner-drawn text has the
  same "works because unthemed" property as the progress bar. Re-check.
- GroupBox/Box `ContainerCtlProc` interior erase: still needed (a themed group box is
  background-transparent and the pane is `WS_CLIPCHILDREN`), but re-verify.

### 5.4 The dead custom-draw button test

Under v6 a `BUTTON` DOES send `NM_CUSTOMDRAW`, so `tryCustomDrawButton` starts firing
for real. Today it never runs in a live window and its self-test passes only because
`nativeTestCustomDrawFill` calls the draw function straight into a memory DC. Once the
manifest lands, make the test drive the real message path so it stops certifying
something that does not happen.

### 5.5 Gates

`gallery.exe --selftest`, `test.bat Release`, `test_lsp.bat`, `example.bat` - plus, and
this is the point of the whole phase, a VISUAL pass in BOTH themes over every example
with a GUI (gallery, fedit, settings, map, 01-elements). The manifest changes the look
of every control in every app, so headless green means nothing here. Use the `scratch/`
probes (see below). Check the size grip and the buttons specifically, since they are the
controls this phase exists to fix.

## Not done / parity gaps

- `Text.font` variants (`FONT_TITLE` / `FONT_CAPTION`) are still Cocoa-only on both
  Windows hosts - see `internal/issue/ui-native-visual-polish-win32-winui.md`.
- macOS: none of this was re-verified on a Mac box (no hardware). The Win32 changes
  are gated to `win32.cb`; only the Phase-4b layout change is host-neutral and it is
  covered by `test.bat` / `example.bat`.

## Verification tooling

Headless suites do NOT catch any of the Phase 1-4 defects (they are pixel bugs, and
the drivers bypass the OS message path). Screenshot probes live in `scratch/`
(gitignored): `dark_scan.ps1` (theme flip + wheel down the page, capture each depth),
`midscroll_probe.ps1` (burst of notches, capture WITHOUT letting the paint queue drain -
this is the scroll-smear check), `ctx_probe.ps1` (real right-click), `geom_probe.ps1`
(dump child rects). A capture probe MUST call `SetProcessDpiAwarenessContext(-4)` or
`GetWindowRect` returns scaled coords and the grab lands off-window.
