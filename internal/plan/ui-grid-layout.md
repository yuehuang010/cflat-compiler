# UI framework: grid layout - flex weights (shipped) + Grid / GridView (proposed)

Status:
- Flex weights + flexWrap: SHIPPED (2026-07-12; d08998a + follow-ons). Record below.
- Grid (track layout) + GridView (virtualized item grid): IMPLEMENTED 2026-08-24
  (ratified same day, all four acceptance points), uncommitted. Grid adopted in
  fedit (both authoring variants; splitH/fixed-size arithmetic removed, app is
  resize-driven) and the gallery (Grid form card + GridView 10k-cell card).
  buildci.bat green over the full set. Deferred as planned: cell span (1b),
  cell recycling (2b), Cocoa/WinUI wheel routing.

## Part 1 (shipped): flex weights + flexWrap

The 1-D layout idiom. `Style.flex` distributes a row/column's leftover main-axis
space by integer weight; `Style.flexWrap` (WRAP_WRAP, DIR_ROW only) wraps
overflowing children into lines with per-line flex distribution. Landed record:

- Phase 1: Style.flex + flexStyle() helper, Element.flexWeight() seam,
  View.layout distribution phase (row + bounded-height column), doc/UI.md "Flex
  weights" section, tui_demo.cb self-tests, gallery reworked onto the
  label+flex-1 form idiom.
- Phase 2: Style.flexWrap + WRAP_NONE/WRAP_WRAP, DIR_ROW wrap branch (measure ->
  greedy line-break -> per-line flex distribution, mirroring the non-wrap
  algorithm one line at a time); DIR_COLUMN ignores flexWrap (documented no-op).
- Follow-on: native ScrollView on the Cocoa host (ELEM_SCROLL -> real
  NSScrollView, flipped document view, framework scrollY stays 0, one nesting
  level) plus layoutRootBounded so native roots lay out with bounded height.
  Gallery remade as a scrollable column of cards.
- All .cb, zero compiler changes. Verified on mac (test.sh, example_mac.sh,
  tui_demo self-test, gallery selftest) and Windows gates.

Key contracts that part 2 builds on:
- Distribution arithmetic: leftover = avail - fixed - gaps, floored at 0; shares
  by weight with cell-at-a-time remainder correction so shares sum exactly.
- Flex-child contract: the container passes the share as the child's availW AND
  advances the cursor by the share regardless of the returned Size (leaves size
  intrinsically; the advance is what aligns columns).
- Column flex only under bounded availH; unbounded falls back to intrinsic.
- Explicit style.width/height on a child wins over flex (ratified 2026-08-24).
- Re-stamping bounds by a second layout call is legal.

## Part 2 (proposed): Grid + GridView

Flex/flexWrap stay the default idiom for 1-D layout; Grid is additive. The gap
flex cannot close is cross-row column alignment of heterogeneous children:
"column 2 is as wide as its widest cell across ALL rows". Today that is faked
with hand-tuned fixed label widths, which break the moment content or font
changes. Every mature toolkit answers this with a track grid (WPF Grid, CSS
Grid, GTK GtkGrid, Qt QGridLayout, AppKit NSGridView, SwiftUI Grid) - it is the
established equivalent under the ratified layout doctrine ("standard
constraints-down/sizes-up model; adopt the established equivalent, do not
invent bespoke rules").

### Two components, two problems

- **Grid** - a layout primitive: a fixed, heterogeneous set of children arranged
  into declared row/column tracks with cross-row alignment. The settings-form /
  dialog case. No scrolling, no virtualization.
- **GridView** - an item view: N uniform cells from a callback data source, with
  its own scrolling viewport and on-demand realization so `count` can be 100k+
  and scrolling stays smooth. The photo-gallery / icon-grid case. The 2-D
  sibling of ListView, and deliberately shaped like it (callback source,
  controlled selection, owning closures).

Do not conflate them: Grid gets no scrollbar, GridView gets no per-track sizing.

### Phase 1: Grid

#### Public surface

```
interface IGrid : IElement
{
    void add(adopt IElement child);              // fills row-major
    void colFixed(int col, int widthCells);      // track overrides; default = auto
    void colStar(int col, int weight);
    void rowFixed(int row, int heightCells);
    void rowStar(int row, int weight);
    int  gap;                                    // cells between tracks (both axes)
};

move IGrid grid(int cols);                        // all-auto tracks
move IGrid grid(int cols, Style style);
```

- Track sizing kinds (per column, per row): **auto** (default; max intrinsic
  size across the track's cells), **fixed** (explicit cells), **star** (weight
  over the remainder - the existing flex distribution, per axis).
- Children fill row-major; row count = ceil(children / cols). Trailing partial
  row is legal.
- Cell content alignment: the child is laid out with the cell as its constraint
  box (availW = track width, exactW = true so containers fill; leaves size
  intrinsically inside the cell, matching the flex advance-by-share contract).
- **Cell span: deferred** (phase 1b at the earliest). Every toolkit has it, but
  it complicates auto-track measurement (a spanning cell's size must distribute
  across its tracks) and the form/dialog case does not need it. Record the gap
  in doc/UI.md; add later without breaking the surface (e.g.
  `g.addSpan(child, colSpan)`).

#### Layout algorithm (columns pass, then rows pass)

Structurally the part-1 flex loop run twice, once per axis:

1. Measure: probe-layout each auto-track cell to learn intrinsic sizes; an auto
   track's size = max across its cells.
2. Distribute: remainder = avail - fixed - auto - gaps, floored at 0; star
   tracks share it by weight, cell-at-a-time remainder correction so shares sum
   exactly (same arithmetic as View.layout flex distribution).
3. Place: final layout call per child at its cell origin with the cell's
   constraint box.

Rows: same algorithm on the height axis. Star rows only when availH is bounded;
under LAYOUT_UNBOUNDED, star rows fall back to auto (documented, mirrors the
column-flex bounded-height rule from part 1).

Precedence composes with the ratified rules, in order: explicit child
style.width/height wins inside its cell; then the cell box; then (when the
minmax p4 lands) min/max clamps as the final per-element step. A fixed track
crops an oversized child via normal clip behavior; it does not grow.

#### What Grid is NOT

- Not a host control. Containers are invisible to the native hosts (win32/
  cocoa/winui position leaf controls at nodeBounds()). Grid is pure
  ui_native.cb math - zero host work, zero compiler work, no new ELEM_ kind
  needed by hosts (a new ELEM_GRID constant for kind()/reconcile only).
- Not CSS Grid: no named lines, no areas, no auto-placement modes, no minmax()
  track functions. Tracks are auto | fixed | star, period.

### Phase 2: GridView (on-demand realization, smooth scrolling)

#### Public surface (ListView-shaped)

```
interface IGridView : IDisableable
{
    int  cellW;                        // uniform cell size in cells (required)
    int  cellH;
    int  count;                        // total items; 100k+ is the design point
    Lambda<IElement(int)> makeCell;    // realize item i -> element (owning closure)
    int  selectedIndex;                // controlled; -1 = none
    Lambda<void(int)> onSelect;
    Lambda<void(int)> onActivate;      // double-click / Enter
    int  width;                        // viewport, like ListView
    int  height;
};

move IGridView gridView(int cellW, int cellH, int count, Lambda<IElement(int)> makeCell);
```

- Columns-per-row = max(1, innerW / (cellW + gap)) - derived, never declared.
  Resize reflows automatically (the Flutter GridView / LazyVGrid model).
- `makeCell` is the data source, exactly as `rowText` is ListView's: called on
  demand for visible items only, an owning closure capturing the component
  pointer so it reads current state across committed-node reuse.

#### Virtualization model

GridView owns its viewport and scrollY (self-scrolling, like ScrollView's key
handling plus wheel), rather than cooperating with an enclosing ScrollView.
Rationale: ScrollView lays out ALL children every pass and has no viewport-
intersection protocol; adding one would touch every container for one consumer.
Self-scrolling keeps the change local and mirrors how ListView already owns its
own scrolling on every host. Nesting a GridView inside a ScrollView is
documented as unsupported (same one-level rule the native ScrollView mapping
already has).

Realization per layout pass:

1. Compute visible row range from scrollY, viewport height, cellH, plus an
   overscan of OVERSCAN_ROWS (start at 2) above and below - the smooth-scroll
   margin: a one-row scroll step never realizes on the paint-critical path
   because the next row already exists.
2. Realized cells live in a keyed pool `dictionary<int, IElement>` (item index
   -> element). Entering items: `makeCell(i)`, layout at cell bounds. Leaving
   items: destroyTree + delete (simple eviction first; see recycling below).
   Scrolling by one row therefore touches one row's worth of cells, not
   `count`.
3. Layout/paint/dispatch only iterate the pool, clipped to the viewport via the
   existing ICanvas clip seam.

Recycling (phase 2b, only if profiling demands it): reuse an evicted element
for an entering item by re-propping through the reconciler's existing patch
path instead of destroy+create. On native hosts create/destroy churns real
controls, so this matters there first. Do NOT build it speculatively - the
overscan margin plus cheap TUI/canvas cells may make eviction fine; measure
with a 100k-item gallery card before adding the complexity.

Smooth scrolling semantics, per host:
- TUI/canvas: cell-row granularity (the grid is character cells; that IS smooth
  there).
- Native hosts: wheel scrolls in cell rows for v1. Pixel-level inertial scroll
  is host-owned scrolling (NSScrollView-style) and explicitly out of scope; if
  demanded later it follows the native-ScrollView mapping pattern, one host at
  a time, without changing the portable surface.

#### Reconciler and ownership

- New ELEM_GRIDVIEW kind; propsEqual compares cellW/cellH/count/width/height +
  style; makeCell re-boxed across the host seam the way rowText is (ListRowBox
  idiom).
- The realized-cell pool is owned by the committed node; destroyTree drains it.
  Realized elements are internal - they do not appear in toJson (toJson emits
  props + count, not children), so headless assertions target the pool through
  a probe (see testing) rather than the JSON tree.

### Interaction with open p4 items

- `internal/issue/p4/ui-native-minmax-clamps.md`: unchanged and still wanted;
  inside Grid, clamps act per-element after cell-box resolution - same "last
  step" rule, no special casing. Land order is independent.
- `internal/issue/p4/ui-context-window-state-exposure.md`: unrelated; untouched.

### Testing (no new test files)

- example/ui/02-terminal/tui_demo.cb self-tests: auto-track = widest cell
  across rows; fixed + star + gap arithmetic incl. remainder correction;
  trailing partial row; star-row bounded/unbounded fallback; explicit-child-
  width beats cell box; GridView: visible-range math at several scrollY values,
  pool size == visible + overscan (never count), scroll-by-one realizes exactly
  one row, eviction on range exit, selection + onActivate.
- example/ui/05-gallery: one "Grid" card (form with auto label column + star
  field column - the case flex could not do) and one "GridView" card (10k
  cells, scroll it in the selftest, assert pool stays bounded). Selftest count
  grows in place.
- Hardening: resizeStorm already in UiTestSuite covers reflow-on-resize for
  both.

### Docs

- doc/UI.md: "Grid (track layout)" section next to the flex rules - track
  kinds, precedence order, row-major fill, span deferred; "GridView
  (virtualized item grid)" next to ListView - callback contract, derived
  columns, overscan, the no-nested-ScrollView rule.
- example/ui/README.md: gallery chapter mentions both cards.

### Phasing and gates

1. Phase 1 Grid (layout only) - land alone, all .cb, zero compiler changes.
2. Phase 2 GridView (virtualization) - depends on nothing in phase 1 except the
   doc structure; can follow immediately.
3. Phase 1b span / phase 2b recycling - only on demonstrated need.

Each phase: buildci.bat green on Windows (build + test.bat + test_lsp.bat +
example.bat incl. gallery selftest); deploy edited core to x64/<Config>/core/
when iterating. Host-neutral .cb changes only; mac verification owed to the
maintainer's mac box as usual.

### Acceptance

Maintainer ratifies before implementation:
1. The two-component split (Grid vs GridView) and both factory spellings.
2. Track kinds limited to auto | fixed | star; span deferred.
3. GridView self-scrolls (no ScrollView cooperation protocol) and derives its
   column count from viewport width.
4. Eviction-first virtualization with overscan; recycling only after profiling.
