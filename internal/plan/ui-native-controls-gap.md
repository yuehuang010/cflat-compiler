# ui-native controls gap: commonly used controls not yet in the framework

Opened 2026-08-24. INVENTORY + STAGING PLAN, multi-change. Extends [[ui-native-framework]] and
[[macos-gui-cocoa]]; sits alongside [[ui-win32-native-polish]] and [[ui-visual-polish]]. The
question this answers: measured against the standard toolkits (Win32 common controls, AppKit,
WinUI 3), which commonly used controls does ui-native still lack, and in what order should they
be added?

## Status update 2026-08-24: Tier 1 LANDED (macOS-verified; Windows verification pending)

All Tier-1 rows below are implemented, with the search box delivered as the `placeholder`
prop (native search class deferred as planned). Shipped spellings:

- TextInput: `password` bool (creation-time - the host picks the native class at create;
  flipping it on a live control does not re-class) and `placeholder` (live-synced).
- ProgressBar: `indeterminate` bool (live-synced, PROP_INDETERMINATE).
- Dialogs (host free functions via host.cb, like nativeOpenFile): `int nativeMessageBox(
  string title, string text, int kind)` with `MSGBOX_OK/MSGBOX_OKCANCEL/MSGBOX_YESNO`
  (returns 1 = first button), `string nativeSelectFolder()` ("" = cancel). Headless
  scripting: `uiTestScriptMessageBox(int)` / `uiTestScriptFolder(string)` (host-side queues).
  WinUI has no dialogs (parity with open/save).
- New kinds: ELEM_SPINNER=26 `spinner(value, min, max, onChange)` (ISpinner, `step` field;
  cocoa NSTextField+NSStepper composite, win32 updown+edit buddy, winui NumberBox);
  ELEM_LINK=27 `link(text, onClick)` (fires onClick only - never opens URLs itself; cocoa
  attributed borderless NSButton, win32 SysLink, winui HyperlinkButton); ELEM_TOOLBAR=28
  `toolbar(onCommand)` + addButton(label, cmd)/addSeparator(), per-item tips/enabled -
  TEXT buttons v1, icons are a follow-up (cocoa composite button row, win32 ToolbarWindow32,
  winui StackPanel of Buttons). Drivers: spinnerSet/linkClick/toolbarClick.
- Gallery restructured into a Tab Control of pages (Basics, Input, Data, Layout, Media,
  Chrome, Stress), one `_pageXxx(ctx)` method per page; selftest stays ONE case (WinUI
  relaunch limit) and visits every page via nativeTabSelect; 78/78 assertions.

Verified on macOS: gallery selftest 78/78, scratch probes green, test.sh Release all pass.
The win32.cb/winui.cb sides are written but CANNOT compile on a macOS host - Windows
verification (test.bat + example.bat + gallery selftest) is the remaining step before
Tier 1 is fully closed. WinUI keeps its pre-existing pattern of driver-only event wiring,
and password there stays TextBox with a TODO until PasswordBox property routing exists.

## Current coverage (baseline as of e95aa98)

25 ELEM kinds in `cflat/core/ui_native.cb`: View, Text (label), Button, Box, TextInput,
TextArea, Checkbox, Radio/RadioGroup, Combo (dropdown-list), Slider, Progress (determinate),
StatusBar, Scroll, ListView, Tree, Tabs/TabPane, Split, Image, GroupBox, Canvas, Grid (layout),
GridView, Component. Plus non-tree surface: MenuBar (menuAddTop/menuAddItem), ContextMenu,
tooltips (ITooltipped on 15 classes), open/save file dialogs (nativeOpenFile/nativeSaveFile),
timers, and the ui_test headless driver.

Confirmed absent (grepped, not assumed): password input style, indeterminate/marquee progress,
message box, folder picker, editable combo, secondary windows.

## Tier 1 - used by almost every non-trivial app

| Control | Win32 | Cocoa | WinUI 3 |
|---|---|---|---|
| Spinner / numeric up-down | `msctls_updown32` + edit buddy | NSStepper + NSTextField | NumberBox |
| Password input | `ES_PASSWORD` edit style | NSSecureTextField | PasswordBox |
| Search box (cue text + clear) | edit + `EM_SETCUEBANNER` | NSSearchField | AutoSuggestBox |
| Hyperlink label | SysLink | NSButton link style | HyperlinkButton |
| Toolbar (icon buttons + separators) | ToolbarWindow32 | NSToolbar | CommandBar |
| Indeterminate/busy progress | `PBS_MARQUEE` | spinning NSProgressIndicator | ProgressRing |
| Message box / confirm dialog | MessageBoxW | NSAlert | ContentDialog |
| Folder picker | IFileDialog + FOS_PICKFOLDERS | NSOpenPanel dirs-only | FolderPicker |

Cost notes:

- **Password input** and **indeterminate progress** are nearly free: both are a style/flag
  variant of a control every host already creates (TextInput, Progress). Model them as a
  bool prop on the existing element (`password` on TextInput, `indeterminate` on Progress),
  NOT as new ELEM kinds - no new host create-path, just a prop sync.
- **Message box** and **folder picker** are dialog-API additions in the same family as
  `nativeOpenFile`/`nativeSaveFile` - host functions, not tree elements. The ui_test driver
  needs a scripted-answer hook so selftests stay headless.
- **Search box** can start as TextInput + `placeholder`/`cue` prop (also generally useful)
  and graduate to the native search class per host later without an app-facing change.
- Spinner, hyperlink, and toolbar are genuinely new ELEM kinds (full 3-host cost).

## Tier 2 - common in settings/document apps

| Control | Win32 | Cocoa | WinUI 3 |
|---|---|---|---|
| Toggle switch | owner-drawn (checkbox fallback) | NSSwitch (10.15+) | ToggleSwitch |
| Date picker | `SysDateTimePick32` | NSDatePicker | DatePicker / CalendarDatePicker |
| Editable combo | ComboBox `CBS_DROPDOWN` | editable NSComboBox | editable ComboBox |
| Segmented control | none native (button row) | NSSegmentedControl | SelectorBar / RadioButtons row |
| Expander / disclosure section | owner-drawn header | NSDisclosureButton + collapse | Expander |
| Color well + color dialog | ChooseColor | NSColorWell / NSColorPanel | ColorPicker |
| Secondary windows / modal sheet | CreateWindow + modal loop | NSWindow sheet / NSPanel | Window / ContentDialog |

Notes:

- **Editable combo** is a prop on the existing Combo (`editable`), not a new kind.
- **Toggle switch** and **segmented control** have no native Win32 class; the Win32 host
  either owner-draws or falls back (checkbox / radio row). Decide the fallback policy before
  building - visual parity vs native purity.
- **Secondary windows** are an architecture item, not a control: the declarative model,
  event routing, and the ui_test driver are all single-window today. Scope separately if
  pursued; everything else in this file fits the current one-window model.

## Tier 3 - heavier or niche; needs a maintainer ruling before any work

- **Rich text editor** (RichEdit / NSTextView attributed / RichEditBox). Big surface: the
  declarative model needs an attributed-run representation and a change protocol. Do not
  start without a plan file of its own.
- **WebView** (WebView2 / WKWebView). Cheap on macOS, a packaging/runtime dependency on
  Windows (WebView2 runtime). Ruling needed on whether ui-native takes that dependency.
- **Popover/flyout** (anchored popup: NSPopover / WinUI Flyout; Win32 has no primitive -
  needs an owned borderless popup window).
- **Font picker dialog** (ChooseFont / NSFontPanel).
- **InfoBar / inline notification banner** (WinUI-native; owner-drawn elsewhere).

Deliberately excluded (legacy Win32 controls modern apps do not use): Rebar, Animation
control, IP-address edit, HotKey control, Pager.

## Cross-cutting costs and sequencing

Every new ELEM kind costs: ui_native.cb element class + serialization, 3 host
implementations (win32.cb, cocoa.cb, winui.cb), ui_test driver hooks, a doc/UI.md parity
matrix row, and gallery example coverage. Prop-variants (password, indeterminate, cue text,
editable combo) skip most of that - prefer them wherever a control is a styled sibling of an
existing one.

Suggested batches:

1. **Prop-variant batch** (cheapest, immediate app value): TextInput `password` +
   `placeholder`, Progress `indeterminate`, Combo `editable`.
2. **Dialog batch**: message box (info/confirm/yes-no), folder picker; scripted answers in
   ui_test.
3. **New-kind batch A**: Spinner, Hyperlink, Toggle switch (with the Win32 fallback ruling).
4. **New-kind batch B**: Toolbar, Segmented control, Expander.
5. **Tier 2 remainder** (date picker, color well) and any Tier 3 item only after its own
   ruling; secondary windows as a separate architecture plan if wanted.

CFlat-side spellings (element names, prop names, dialog function signatures) need the
maintainer's ratification per the `internal/issue/p4/` convention before a batch starts;
this file is the shared inventory so each batch does not need its own p4 entry for scoping.
