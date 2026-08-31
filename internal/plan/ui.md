# UI framework (ui_native): consolidated plan

Consolidated 2026-08-31 from five plans that were each mostly landed record:
`ui-grid-layout.md`, `ui-map-canvas.md`, `ui-native-controls-gap.md`,
`ui-win32-native-polish.md`, `macos-gui-cocoa.md` (all deleted). Two more UI plans
were deleted the same day as fully landed with nothing open: `ui-native-framework.md`
(P0-P14, framework promoted to `core/ui_native` 2026-07-07) and `ui-visual-polish.md`
(phases A+B+C, 2026-07-12). `ui-test-framework.md` (T1-T3) and `ui-goals.md` went too.

**doc/UI.md is the record of what shipped.** This file carries only what is still
open, plus the contracts open work depends on.

## Landed ledger (one line each - detail in doc/UI.md)

| Item | Landed |
|---|---|
| Framework P0-P14, promoted to `core/ui_native` (Win32 + Cocoa + WinUI 3 hosts, fedit, gallery) | 2026-07-07 |
| ui_test headless driver + `test_example.bat --worker-uitest` gate + `example/ui/testing/` template | 2026-07-07 |
| Flex weights + flexWrap (`Style.flex`, `Style.flexWrap`), native Cocoa ScrollView | 2026-07-12 (d08998a) |
| Visual polish A+B+C: container backdrops, typography, gallery remake (Cocoa) | 2026-07-12 |
| Map canvas M1-M4: interactive CanvasView, canvas image handles, tile engine, MapView + example | 2026-07-12 |
| Win32 host phases 1-4b: scroll repaint storm, container paint, context menu, dark theming, Box/GroupBox insets | 2026-07-13..08-14 |
| Win32 phase 5: comctl32 v6 manifest (via the typed manifest system), InitCommonControlsEx, NM_CUSTOMDRAW | 2026-08-14 |
| Grid (track layout) + GridView (virtualized item grid), adopted in fedit + gallery | 2026-08-24 |
| Native controls Tier 1, Tier 2 (except secondary windows), Tier 3 - all rows | 2026-08-24..25 |
| Windows-side verification of every tier (test.bat + test_example.bat + gallery selftest) | 2026-08-25 |
| macOS Cocoa stages 1-2: framework linking, Darwin triple for header binding | complete |

Residual defects live in `internal/issue/ui/`, not here (e.g.
`win32-trackbar-dark-channel-light.md`, `ui-native-visual-polish-win32-winui.md`).

## Open 1: secondary windows / modal sheets

The one Tier-2 row never built, and the largest single gap. Needs its own
architecture plan before any code - it touches the host globals every existing
self-test relies on (the same hazard the P7 multi-window refactor hit). Not a
control; do not fold it into a controls batch.

## Open 2: map canvas M5 - polish (needs the timer seam)

M1-M4 landed 2026-07-12; M5 was never started.

- Host timer seam: `u64 hostStartTimer(int ms, bool repeating, closure)` /
  `hostCancelTimer(u64)` - NSTimer on Cocoa, SetTimer on Win32; boxed closure
  like `ctx.post`. Everything else in M5 depends on this.
- Inertial panning (velocity sample on pointer-up, decay ticks), animated zoom
  (eases toward target over ~150ms), tile retry backoff.
- Overlays: marker list (world-anchored, drawn post-tiles, hit-testable ->
  onMarkerPress), polyline layer. HUD zoom +/- buttons are ordinary Button
  elements floated over the canvas - needs no new machinery if the map region
  and buttons are siblings in a column.
- Deferred beyond M5: rotation/tilt, vector tiles, text collision, network tile
  sources (an http example can come later - the socket layer exists).

Gate: all .cb, no compiler C++ expected. Map example joins the sweep; it SKIPs on
Win32 until the canvas-input parity issue is closed.

Trap worth keeping: `cache` is a soft keyword (the `import ... cache;` clause) and
cannot be used as a variable name. Used as a local inside a lambda body it reports a
confusing ANTLR error at the lambda's `=>`, not at the offending line. The MapView
field is named `tileCache` for this reason.

## Open 3: Grid / GridView deferred sub-items

Deferred as planned when Grid + GridView landed 2026-08-24, each gated on
demonstrated need:

- **1b - cell span.** Track kinds stay `auto | fixed | star`; span was ruled out of
  the first cut.
- **2b - cell recycling.** GridView is eviction-first with overscan; recycling only
  after profiling says the pool churn matters.
- **Cocoa / WinUI wheel routing** for GridView (Win32 only today).

Contracts these build on (ratified 2026-08-24, do not re-derive):
- Distribution arithmetic: `leftover = avail - fixed - gaps`, floored at 0; shares by
  weight with cell-at-a-time remainder correction so shares sum exactly.
- Flex-child contract: the container passes the share as the child's availW AND
  advances the cursor by the share regardless of the returned Size.
- Column flex only under bounded availH; unbounded falls back to intrinsic.
- Explicit `style.width`/`height` on a child wins over flex.
- Re-stamping bounds by a second layout call is legal.
- GridView self-scrolls (no ScrollView cooperation protocol) and derives its column
  count from viewport width; no nested ScrollView.

## Open 4: cross-host parity gaps

- `Text.font` variants (`FONT_TITLE` / `FONT_CAPTION`) are Cocoa-only; both Windows
  hosts lack them - `internal/issue/ui/ui-native-visual-polish-win32-winui.md`.
- The Win32 phase 1-4 work was never re-verified on a Mac box. Those changes are
  gated to `win32.cb`; only the phase-4b layout change is host-neutral and it is
  covered by `test.bat` / `test_example.bat`.
- Confirmed absent from the element set (grepped, not assumed) beyond the tiers
  already built: nothing except secondary windows (Open 1).

## Open 5: macOS Cocoa stage 3 remainder

Stages 1 (framework linking) and 2 (Darwin triple for header binding) are complete.
Stage 3 - a curated `core/cocoa.cb` with cflat lifetime semantics - is partly
overtaken by the `ui_native` Cocoa host, but these items were never done:

- Map retain/release onto cflat ownership: wrapper struct holding the objc id,
  destructor sends `release`, copy/assign sends `retain` (or is move-only). Follow
  the single Cocoa convention: alloc/new/copy => owned (+1), everything else
  borrowed.
- Autorelease pool management: push in `cocoaApp()`, and around any helper that runs
  before `[NSApp run]` takes over per-event pool draining.
- Decide whether the bridge is promoted from `example/macos/cocoa.cb` into
  `core/cocoa.cb` (gated with `if const (__MACOS__)` like `os.posix.cb`) or stays a
  blessed example lib.

**Stage 4 (Obj-C header binding) is deferred, likely never.** Parsing AppKit.h needs
`-x objective-c`, framework include resolution, and a design for what an `@interface`
becomes in cflat. The curated library makes it unnecessary. New Apple APIs are
increasingly Swift-only (no C surface), so full coverage is unreachable regardless;
the Obj-C tier itself is stable/frozen.

Cocoa facts worth keeping in mind:
- arm64 requires the typed-function-pointer cast of `objc_msgSend` even in C; cflat
  loses nothing vs clang. No `objc_msgSend_stret` on arm64.
- NSRect (4 doubles) is an HFA in d0-d3: register-identical to 4 separate double
  args, so `initWithContentRect:` needs no by-value struct support.
- NSWindow defaults to `releasedWhenClosed=YES` - wrappers must set NO or the close
  button dangles our pointer.
- Cocoa coordinates are bottom-left origin, y grows upward (opposite GDI).
- The Obj-C memory model is refcounting; ARC is a clang compile-time feature, absent
  at the msgSend level - manual rules apply, which the destructor mapping hides.

## Cost model for any new control

Every new ELEM kind costs: `ui_native.cb` element class + serialization, 3 host
implementations (`win32.cb`, `cocoa.cb`, `winui.cb`), ui_test driver hooks, a
doc/UI.md parity matrix row, and gallery example coverage. Prop-variants (password,
indeterminate, cue text, editable combo) skip most of that - prefer them wherever a
control is a styled sibling of an existing one.

CFlat-side spellings (element names, prop names, dialog function signatures) need the
maintainer's ratification per the `internal/issue/p4/` convention before a batch
starts.

## Verification gates (all UI work)

Host-neutral and Cocoa-host `.cb` changes: on the mac box `tui_demo.cb` self-test,
gallery `--selftest`, fedit `--selftest`, `./test.sh Release`, `./test_example.sh`,
plus a live launch for visual confirmation. On Windows: `buildci.bat` green over the
full set (build + `test.bat` + `test_lsp.bat` + `test_example.bat` incl. gallery
selftest). Deploy edited core files to `x64/<Config>/core/` when iterating - the
compiler loads core next to the exe.

**Headless suites do NOT catch pixel bugs** (the drivers bypass the OS message path).
Screenshot probes live in `scratch/` (gitignored): `dark_scan.ps1` (theme flip + wheel
down the page, capture each depth), `midscroll_probe.ps1` (burst of notches, capture
WITHOUT letting the paint queue drain - the scroll-smear check), `ctx_probe.ps1` (real
right-click), `geom_probe.ps1` (dump child rects). A capture probe MUST call
`SetProcessDpiAwarenessContext(-4)` or `GetWindowRect` returns scaled coords and the
grab lands off-window.
