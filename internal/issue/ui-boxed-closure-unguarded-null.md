# Boxed read-as-value closures segfault when unset (rowText / childCount / onPaint)

Created: 2026-07-14 (found while fixing button-fireclick-unguarded-null-handler)

## Summary

Same crash class as the `fire*` null-handler bug just fixed, one seam deeper and NOT
covered by that fix.

The five event closures that are READ AS VALUES rather than invoked through a `fire*`
wrapper - `ListView.rowText`, `TreeView.childCount` / `childId` / `label`, and
`CanvasView.onPaint` - are boxed (`_boxListRow`, `_boxTree`, `_boxCanvas`) and handed to
the host as an opaque `u64`. The box-invoke helpers guard the BOX POINTER but never the
CLOSURE INSIDE the box, so a control built via tag sugar without the closure set boxes a
NULL closure and the first callback jumps through a null function pointer.

The `boxPtr == 0u` guard gives false confidence: the box is a real heap object, so the
guard passes, and the null is one level in.

## Repro (verified, exits 139 / SIGSEGV)

```cflat
import "ui_native.cb";

extern int main()
{
    // ListView built without rowText - exactly what tag sugar permits.
    ListView* lv = new ListView();
    u64 box = (u64)_boxListRow(lv.rowText);
    printf("box=%llu (non-zero, so the boxPtr guard passes)\n", box);
    string s = __uiListRowText(box, 0, 0);   // host callback path -> SEGFAULT
    printf("survived: %s\n", s.data());
    return 0;
}
```

Actual output:

```
box=105553150214176 (non-zero, so the boxPtr guard passes)
<segfault, exit 139>
```

The equivalent tag-sugar spelling that reaches this is `<ListView rowCount="5"/>` with no
`rowText`, and the same shape for `TreeView` / `CanvasView`.

Note this reproduces with NO GUI host involved, so it is fully testable on any platform.

## Root cause

Two distinct unguarded paths, both in `cflat/core/ui_native.cb`:

**1. The box-invoke helpers (host seam).** Each guards `boxPtr` and then calls the closure
field unconditionally:

```cflat
// :3161  __uiListRowText
if (boxPtr == 0u) return "";
ListRowBox* b = (ListRowBox*)boxPtr;
return b.cb(row, col);              // <-- b.cb may be null

// :3666  __uiTreeChildCount   (and __uiTreeChildId :~3673, __uiTreeLabel :~3680)
if (boxPtr == 0u) return 0;
TreeBox* b = (TreeBox*)boxPtr;
return b.childCount(nodeId);        // <-- b.childCount may be null

// :4029  __uiCanvasPaint
if (boxPtr == 0u) return;
CanvasBox* b = (CanvasBox*)boxPtr;
b.paint(c);                         // <-- b.paint may be null
```

**2. The in-core `paint()` fallbacks (no host involved).** These call the same closures
directly:

- `ListView.paint`   (~:3062) - `line = line + this.rowText(r, cc);`
- `TreeView.paint`   (~:3429-3436) - `this.childCount(...)` / `this.childId(...)` / `this.label(...)`
- `CanvasView.paint` (:3991) - `void paint(ICanvas cv, IUiContext ctx) { this.onPaint(cv); }`

## Fix direction

The idiomatic closure null test works and is already used by the `fire*` wrappers as of the
button fix:

```cflat
if (this.onPaint != nullptr) { this.onPaint(cv); }
```

So guard both paths. For the box-invoke helpers the guard belongs INSIDE, after the box
cast, and each needs a sensible empty result matching its return type (`""` for `rowText`
and `label`, `0` for `childCount` / `childId`, nothing for `paint`) - i.e. the same value
the existing `boxPtr == 0u` arm already returns. That keeps a handler-less control
rendering as empty rather than crashing.

Open question worth deciding: for `ListView`/`TreeView` a null `rowText`/`label` arguably
indicates a MISUSE (a list with no row text is useless), so an alternative is to reject it
at construction. But `CanvasView.onPaint` being unset is perfectly reasonable (an
input-only canvas), so at minimum that one must be guarded rather than rejected. Guarding
all five is the consistent, lowest-risk choice.

## Related

- `internal/issue/button-fireclick-unguarded-null-handler.md` (FIXED, commit a1f18d0) -
  guarded the 13 `fire*` wrappers. This issue is the remainder that fix deliberately did
  not cover.
