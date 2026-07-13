# UI framework: flex weights (grid-style layout support)

Status: PHASE 1 IMPLEMENTED (2026-07-12), uncommitted. Phase 2 (flexWrap) not started.

- Landed: Style.flex + flexStyle() helper, Element.flexWeight() seam, View.layout
  distribution phase (row + bounded-height column), doc/UI.md "Flex weights"
  section, 6 new tui_demo.cb self-tests (14/14), gallery reworked onto the
  label+flex-1 form idiom plus a new "Flex layout" card (selftest 27 -> 30).
- All .cb, zero compiler changes. Edited core deployed to x64/Release/core/.
- Verified on the arm64 mac box: test.sh Release 160/0, example_mac.sh 34/0,
  live gallery window launched and closed cleanly (exit 0).
- Owed before commit: test.bat + example.bat on the Windows box (host-neutral
  .cb change; Win32/WinUI runtime coverage).

## Reframe (v2)

v1 of this doc proposed a public `Grid : Element` container (WPF-style tracks).
REJECTED after direction from the user: the framework's public vocabulary is the
React Native construct set, mapped onto Windows/mac native controls. RN has no Grid
component - grid-like layout in RN falls out of FLEX WEIGHTS (flexGrow) and wrap.
"Grid" is an implementation detail of the layout pass, not a new element kind.

So: no new Element, no new ELEM_ kind, no JSX tag. The feature is new Style props
implemented inside View's existing one-pass layout.

## What is actually missing

View's DIR_ROW/DIR_COLUMN stacking places children at intrinsic size only. There is
no way to say "this child takes the remaining space" or "these two children split
the row 1:2". That single gap is why forms currently hard-code widths. With RN
`flex` weights, the canonical form row is:

```
row:  [ label (width 120) ] [ field (flex 1) ]
```

and column alignment across rows comes from the shared fixed label width - exactly
the RN idiom. A 2-of-3 / 1-of-3 split is `flex 2` next to `flex 1`. A card grid is
a wrapping row of fixed-size cells (`flexWrap`, phase 2).

## API: Style additions (RN names)

```
struct Style
{
    ... existing fields ...
    int flex = 0;       // main-axis weight; 0 = intrinsic size (today's behavior)
    int flexWrap = 0;   // WRAP_NONE | WRAP_WRAP (phase 2)
};
```

- `flex` mirrors RN's `flex: n` (flexGrow); integer weights, 0 = unset.
- Mechanical updates: styleIsEmpty, styleEquals, styleToJson, makeStyle untouched
  (new helper `flexStyle(int flex)` or set the field at the call site).
- Zero-default keeps every existing tree byte-identical (framework invariant).

## Layout: View.layout gains a distribution phase (still one protocol pass)

DIR_ROW with any flex child becomes two internal phases; the external contract
(constraints in, size out, bounds stamped) is unchanged, and a View with no flex
children takes the existing fast path untouched.

1. Measure: lay out each flex==0 child once with probe constraints (real y, x=cx
   tentative) to learn its intrinsic width. Sum fixed widths + gaps.
2. Distribute: leftover = availW - 2*pad - fixedSum - gaps, floored at 0. Each
   flex child's share = leftover * flex / flexSum, remainder distributed one cell
   at a time left-to-right so shares sum exactly to leftover.
3. Place: final pass laying out every child at its real x. Re-stamping bounds by a
   second layout call is legal (bounds are overwritten, nothing accumulates).

Flex-child contract (key decision): the container passes the share as the child's
availW AND advances the cursor by the share, regardless of the Size the child
returns. Leaves size intrinsically (TextInput = value length, Text = string
length), so advancing by the share is what makes columns align even when a leaf
paints narrower than its cell. A flex View fills its availW already, so nested
containers stretch naturally. No leaf changes needed.

DIR_COLUMN: same algorithm on the height axis, but ONLY when availH is bounded.
Under LAYOUT_UNBOUNDED height (the layoutRoot default) flex children fall back to
intrinsic height - documented, matches RN's behavior in an unbounded scroll axis.

Interactions:
- style.width on a flex child wins (explicit beats weight), same as RN flexBasis
  with grow 0 - keep it simple: width set => treated as fixed, flex ignored.
- SplitView/ScrollView/GroupBox children: nothing changes; they lay out their
  children through the same View machinery where applicable.

## Phase 2: flexWrap (card grids)

`flexWrap = WRAP_WRAP` on a DIR_ROW View: children that would exceed availW wrap
to a new line; line height = max child height in that line + gap. Flex weights
within a wrapping row apply per-line (RN semantics). This is the "gallery of
cards" grid. Ship phase 1 (flex) first; wrap is an independent follow-up in the
same layout function.

## Host impact: none

Containers are invisible to the native hosts (win32/cocoa position leaf controls
at nodeBounds(); winui's isNativeKind excludes containers). Flex is pure
core/ui_native.cb math realized through absolute leaf bounds on all three hosts
plus the canvas/TUI hosts. No reconciler changes either: Style is already compared
by styleEquals, which picks up the new fields via the mechanical update.

## Testing (no new test files)

- example/ui/02-terminal/tui_demo.cb self-test suite: headless asserts for
  leftover distribution incl. remainder (100 wide, flex 1/1/1 -> 34/33/33),
  fixed+flex mix (label 120 + flex field), flex 2:1 split, zero-leftover clamp,
  column flex under bounded vs unbounded height, no-flex tree byte-identical
  toJson/layout to today.
- example/ui/05-gallery/gallery_app.cb: add a form section using the
  label-width + flex-1 idiom with bounds asserts; runtime coverage on all hosts
  via existing gates.
- Resize sanity free via the UiTestSuite resizeStorm harness.

## Docs

- doc/UI.md: Style listing + a paragraph in "Layout protocol" (flex distribution,
  the advance-by-share contract, column-flex bounded-height rule).
- example/ui/README.md: mention the form idiom in the gallery chapter.

## Verification gates

Host-neutral .cb change only (no compiler C++): example.bat + test.bat on Windows,
./example_mac.sh + ./test.sh Release on the mac box. Deploy edited core files to
x64/<Config>/core/ when iterating locally (compiler loads core next to the exe).
