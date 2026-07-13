# UI framework: visual polish (professional-looking native output)

Status: Phases A + B + C LANDED (2026-07-12, Cocoa host, .cb only; C is
example-only, no framework edits). Phase D not started. Follows the flex work
in ui-grid-layout.md. Driven by a live review of the remade gallery on macOS:
the page reads as floating text on a flat window with huge vertical gaps and
no typographic hierarchy.

## Landed (Phases A + B, Cocoa host)

- **A - container backgrounds**: an `ELEM_VIEW` container (view/row/column) with a
  non-zero `style.backgroundColor` now paints a rounded (8pt) layer-backed `NSView`
  backdrop behind its children, keyed by the container's path, with an optional 1px
  `theme.panelBorder`. Rides the same create/reuse/destroy/reposition reconcile as
  leaf controls (handles 0->color / color->0 / color-change transitions); inside a
  `ScrollView` it lives in the document view so it scrolls with its card. New:
  `_containerBg`, `_createBackdrop`, `_syncBackdrop`, backdrop branch in `_applyNode`
  (`core/ui_native/cocoa.cb`). `Box` is unaffected (already a native `NSBox`).
- **B - typography**: `Text` gained `int font` (default `FONT_UI`=0, byte-identical);
  new consts `FONT_TITLE`=2 (bold ~1.3x) and `FONT_CAPTION`=3 (smaller, secondary
  label color unless `Text.color` set) in `ui_native.cb`. `Text.propsEqual` compares
  font; `Text.toJson` elides font when 0 (default trees byte-identical). Cocoa host
  caches `NSFont` variants (`_fontFor`), applies them on ELEM_TEXT create + re-sync
  (`_applyTextFont`), and honors fontId in `measureText`. Canvas/TUI paint is a
  documented no-op; Win32/WinUI fall back to the default font.
- **Parity**: `internal/issue/ui-native-visual-polish-win32-winui.md` records both
  features as Cocoa-only (Win32/WinUI no-op, non-silent).
- **Tests**: tui_demo self-test +1 (`testTextFont`, 19/19); gallery self-test stays
  33/33 (card backdrops key off container paths, not policed by the stress census).
  Gates on the mac box: tui_demo 19/19, gallery 33/33, fedit 15/15, `test.sh Release`
  160/0, `example_mac.sh Release` 34/0. Owed on Windows before commit: test.bat +
  example.bat. A live gallery window launch (visual) is left to the review session.
  Note: gallery cards still use `column` (ELEM_VIEW) so they now show backdrops; the
  Phase C remake will lean on this plus `FONT_TITLE`/`FONT_CAPTION` for the cards.
- **Truncation fix**: a live Phase C gallery run showed every `FONT_TITLE` `Text`
  clipped ("Widget Gallery" -> "Widget") - `Text.layout()` reserved 1 cell/char
  regardless of font. Fixed in two layers: `ui_native.cb` now reserves `ceil(1.3x)`
  cells for `FONT_TITLE`; `cocoa.cb` additionally widens each `ELEM_TEXT` frame to
  the real CoreText-measured glyph width (`CTLineGetTypographicBounds`, a scalar
  return - `-sizeWithAttributes:`'s `NSSize` struct return is not attempted through
  the msgSend bridge) whenever it exceeds the layout estimate, on create and re-sync.
  `testTextFont` extended to assert a `FONT_TITLE` cell is wider than the plain one.

## Landed (visual bug pass 2, Cocoa host - screenshot-driven)

- **Dark-mode cards stayed light (reconcile drop)**: `reconcile` keeps the committed
  container node and only forwarded a hand-picked set of scalars via
  `adoptContainerProps` (TABS/SPLIT). A theme flip re-colored the WIP `View`'s
  `style.backgroundColor` (=`darkTheme().panelBg`), but that was discarded, so the
  committed `View` kept the stale light fill and `_syncBackdrop` re-applied light
  (white-on-white text, since ELEM_TEXT ink auto-flips with the dark `NSAppearance`).
  Fix: `adoptContainerProps` now copies `a.style = b.style` for `ELEM_VIEW`
  (`ui_native.cb`). `darkTheme()`/`lightTheme()` values were already sensible - no
  color change needed. Oracle: new host-neutral driver `nativeContainerBg(keyPath)`
  (reads the committed `View.style.backgroundColor`; added to cocoa/win32/winui
  mirroring `nativeNodeBounds`) + gallery assert 24b (`== darkTheme().panelBg` after
  the flip). Light mode unchanged (same reconcile path, light values).
- **TabControl pane overlapped the strip (Cocoa framing)**: the `NSSegmentedControl`
  was framed at the TabControl's FULL node bounds, so the strip stretched across the
  whole card and the active pane (laid out below at `y+hdr`) drew on top of it. Fix:
  `_applyNode` frames `ELEM_TABS` at the top `hdr`(=2)-row strip band only
  (`cocoa.cb`); the framework layout already places the pane below. Oracle: gallery
  assert 10b (`paneTxt.y > tabs.y`, pure layout).
- **Context menu dead on real right-click (Cocoa never wired NSMenu)**:
  `nativeSetContextMenu` only stored the boxed `ContextMenu*`; nothing attached an
  `NSMenu` to a view, so only the synthetic `nativeFireContextMenu` worked. Fix:
  `_applyNode` builds an `NSMenu` (items -> `onMenu:` + `setTag:`=cmd, routed through
  `routeMenu` like the menu bar) and `setMenu:`s it on the control at the registered
  path AND its descendants (`_ctxMenuForPathOrAncestor` prefix walk - host views are
  flat siblings that overlap, mirroring Win32 `WM_CONTEXTMENU` bubbling); `cocoa.cb`.
  Live-only for the actual popup (human-verified); the existing synthetic route
  (assert 14) still covers command dispatch, and the code path survives a live launch.
- **Gates (mac)**: gallery 39/39 (was 37, +24b/+10b), tui_demo 23/23, fedit 15/15,
  `test.sh Release` 160/0, `example_mac.sh Release` 34/0. Live gallery launch = no
  crash (3s probe). Owed on Windows before commit: `test.bat` + `test_lsp.bat`
  (nativeContainerBg added to win32/winui but not built/run on the mac box).
- **List/Tree rows tall + text top-aligned (Cocoa cell-based tables, screenshot)**:
  `ELEM_LIST`'s `NSTableView` and `ELEM_TREE`'s `NSOutlineView` never called
  `setRowHeight:`/`setStyle:`, so Big Sur+'s automatic row style inflates row height
  well past the font line height; cell-based `NSTextFieldCell` top-aligns (unlike
  view-based cells, which center) in the extra space. Verified via grep: no
  `setRowHeight`/`heightOfRow`/`setStyle`/`setIntercellSpacing` existed anywhere in
  `cocoa.cb` before this fix. Fix: `msgSetDouble(table/outline, "setRowHeight:", 19.0)`
  pinned right after each control's `alloc`/`initWithFrame:` (`ui_native/cocoa.cb`,
  ELEM_LIST ~line 334, ELEM_TREE ~line 361); `setStyle:` left untouched per the
  enum-uncertainty guidance, relying on the explicit row height alone. Row height is
  a visual metric, not headless-assertable; gates below stayed green and a fresh
  live gallery build launched cleanly. Gates (mac): gallery 39/39, tui_demo 23/23,
  fedit 15/15, `test.sh Release` 160/0, `example_mac.sh Release` 34/0.

## Root causes found in review (screenshot-verified on the Cocoa host)

1. Container backgroundColor is IGNORED by the native hosts. Cards set
   theme.panelBg but containers are not native-mapped, and the Cocoa host only
   consumes color for button accents and the splitter fill. The card design
   silently degrades to unbounded whitespace.
2. Vertical rhythm is coarse: one layout row = 26pt on Cocoa (BASE_Y), so
   gap=1 is 26pt of air and every one-line Text occupies a 26pt row. A
   title+desc+demo card is ~130pt, mostly empty.
3. One font only (FONT_UI). Text has no size/weight/secondary-color option, so
   headers, card titles, and body text all render identically.
4. Test scaffolding leaks into the UI: "(P10 - full element set)" header, raw
   debug status text, the Reconcile Stress card.
5. Composition: buttons stack vertically, image demo is a 12x6-pixel stamp,
   content hugs the left edge with no max width.

## Phase A: container background painting (framework, hosts)

- Cocoa host: when a container element has a non-zero style.backgroundColor,
  create a layer-backed plain NSView behind its children (wantsLayer,
  layer.backgroundColor, layer.cornerRadius ~8pt for rounded cards). The panel
  view is a native control keyed by the container's path; children parent as
  today (absolute positioning is unchanged - the panel is a painted backdrop,
  not a layout parent, EXCEPT inside a ScrollView where it must live in the
  document view so it scrolls with its card).
- Reconciler-visible: styleEquals already compares backgroundColor, so prop
  changes re-sync naturally; the host must handle transitions 0 -> color
  (create) and color -> 0 (destroy).
- Theme: add theme.panelBorder (subtle 1px border color) - optional but cheap
  via layer.borderColor/borderWidth.
- Win32/WinUI parity: record the gap in internal/issue/ (extend the existing
  scrollview parity file or add a sibling); do not block on it.

## Phase B: typography (framework: ui_native.cb + Cocoa host)

- Text gains `int font = FONT_UI;` (0, default). New consts: FONT_TITLE (bold,
  ~1.3x size), FONT_CAPTION (smaller, secondary label color unless style.color
  set). Style/propsEqual/toJson mechanical updates.
- Cocoa host: NSFont systemFontOfSize:weight: variants cached once; measureText
  already takes fontId - honor it there and in setControlText/createControl for
  ELEM_TEXT.
- Canvas/TUI hosts: fontId is a no-op (cell fonts are fixed) - document it.
- Win32/WinUI: no-op fallback to the default font is acceptable initially;
  record in the same parity issue file.

## Phase C: gallery remake on the new primitives (example only) - LANDED

`example/ui/05-gallery/gallery_app.cb` only; zero framework (`core/`) edits.

- Cards: `_card()` now builds a `header` sub-column (gap 0) holding title
  (FONT_TITLE) + desc (FONT_CAPTION), then the card (gap 1) adds the live demo
  after the header. Every card title/desc path grew a `header/` segment (e.g.
  `root/content/cardbtn/header/title`); all self-test paths updated.
- Header row: "Widget Gallery" (P10 codename dropped), FONT_TITLE.
- Buttons demo: `btn` + `themebtn` moved into a `btnrow` row (gap 1) inside the
  Buttons card; paths became `root/content/cardbtn/btnrow/{btn,themebtn}`.
- Status card: human copy - `status` is
  `"Size: <Small|Medium|Large>  Font: <combo item>  Row: <n|none>  Opened: <n|none>"`;
  `status2` is `"Tab: <Alpha|Beta>  Node: <n|none>  Split: <n>%  Ctx: <n|none>"`.
  Asserts 2/3/4/7/8 (`_statusIs` exact match) and 10/12/13/14 (`_statusHas`
  substring) re-pointed at the new copy - same model transitions, new oracle
  text, not weakened.
- Reconcile Stress card: kept the plan's documented fallback - card retitled
  "Stress (self-test)", churn box still gated on `this.stress`, `stressbtn`
  stays always-reachable at `root/content/cardstress/stressbtn` (path
  unchanged) so the self-test drive path (assert 27, reconcileStress) is
  unaffected.
- Page margins: `content` column padding raised 1 -> 3 cols.
- Image demo: gradient buffer raised from 12x6 px to 96x48 px (matches the
  Image element's ~96x104pt Cocoa display size for `width={12} height={4}`
  cells), no key-path/assert impact.
- Self-test: 33 -> 35 asserts (all green). Added #34 (every card's `desc`
  Text exists) and #35 (card-container `nativeHandle` exists on
  `cardbtn`/`cardstatus`, doubling as the Phase A backdrop-existence oracle).
  Skipped a title-font round-trip assert: no host-neutral `nativeControlFont`-
  style query exists yet, so it is not cheaply observable outside Cocoa-
  specific code.

**Max-width gap: CLOSED (2026-07-12, framework pass on `core/ui_native.cb`).**
`View.layout()` now honors its own `style.width`: at the top of the method it
caps the by-value `c.availW` to `style.width` when set, so children lay out
within `style.width - 2*pad` and the View returns `sz.w = style.width`, across
every branch (plain column/row, flex distribution, DIR_ROW wrap). Height stays
content-driven (Box's fixed-`h=1` default is NOT inherited), so a width-capped
`ScrollView`-bearing column still measures its full content height and assert
32's overflow proof holds. When `style.width` is 0 the cap is a no-op
(`c.availW` unchanged), so a no-width tree lays out and serializes byte-
identically - verified by the unchanged flex/wrap self-tests plus a new
`testViewWidthHonored` in `tui_demo.cb` (width-set View advances the cursor by
its width and caps children; width-0 View still fills availW). The gallery
`_card()` now uses `column(makeStyle(1, 70, 0))`, capping every card at ~70 cols
(new gallery assert 37 checks `nodeBounds().w == 70`). `doc/UI.md`'s "Flex
weights" section documents the new self-width behavior.

**Form-label fix (2026-07-12, gallery example).** The three form-row label
wrappers (`inputlabel`/`combolabel`/`flexlabel`) were `box(makeStyle(0, 12, 1))`
- a `Box` maps to a native `NSBox` and insets its children by `1 + padding`, so
with height 1 the label `Text` rendered one full 26pt row BELOW the box (an
empty rounded rect where the label should be, e.g. "Font:" floating under the
combo row). Replaced with plain fixed-width `view(makeStyle(0, 12, 0))` (same
keys, same `text(...)` child): a `View` has no border inset, so the label Text
renders in-row. New gallery assert 36 is the regression check (the label's Text,
at the synthesized `.../combolabel/#0` path, has the same `y` as its row).

## Phase D (deferred): sub-row spacing

A one-line Text will still occupy a full 26pt row. Fixing that needs a
sub-row unit or natural-height text measurement, which breaks the unit-blind
integer layout contract shared with the TUI host. Deferred; gap-0 stacking in
Phase C is the workaround. Revisit only if C still looks sparse.

## Verification gates

Host-neutral + Cocoa-host .cb changes only (no compiler C++). On the mac box:
tui_demo.cb self-test, gallery --selftest, fedit --selftest, ./test.sh Release,
./example_mac.sh Release, plus a live gallery launch for visual confirmation.
Owed on the Windows box before commit: test.bat + example.bat. Deploy edited
core files to x64/Release/core/ when iterating (compiler loads core next to
the exe).

Phase C gate results (mac box, this pass, Release): `out/gallery_mac
--selftest` 35/35, `example_mac.sh Release` 34/0, `test.sh Release` 160/0.
A live (non-selftest) build was compiled to the reviewer's scratchpad
(`gallery_v3`) but not launched - left for the visual-review session. Owed:
Windows `test.bat` + `example.bat` (example-only change touching no compiler
C++, low risk but unverified there), and the live gallery window screenshot
review.
