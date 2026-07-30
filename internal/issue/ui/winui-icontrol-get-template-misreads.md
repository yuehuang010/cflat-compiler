# UNCONFIRMED: IControl::get_Template reads back null on a styled WinUI control

Summary
-------
While diagnosing `winui-unpackaged-missing-resources-pri.md`, a probe through the
winmd-projected `IControl` reported `Template == null` and
`ActualWidth == ActualHeight == 0` for a `Microsoft.UI.Xaml.Controls.Button` that had
been appended to `Canvas.Children` - even after four dispatcher turns. A screenshot of
`winui_demo.exe` then showed a sibling CheckBox rendering FULLY STYLED (box glyph +
label), which proves default templates do load. So at least one of the two reads is
lying.

Cost: this false reading produced a confident-but-wrong root cause ("no control ever
gets a default template; the WinUI backend renders blank") that survived until it was
checked against a screenshot. Do not trust this probe.

Repro (scratch, not committed)
------------------------------
```cflat
void* btn = nullptr;
RoActivateInstance(hstring("Microsoft.UI.Xaml.Controls.Button"), &btn);
((IVector<UIElement>*)_gCanvasVec)->lpVtbl->Append(_gCanvasVec, wqi(btn, iidof(IUIElement)));
// ...after 4 nested winuiPost() turns:
void* ctrl = wqi(btn, iidof(IControl));
void* tmpl = nullptr;
((IControl*)ctrl)->lpVtbl->get_Template(ctrl, &tmpl);   // -> nullptr, but CheckBox renders styled
```

Suspects (in order)
-------------------
1. `IControl` vtable slot offset for `get_Template` is wrong in the generated winmd
   projection, so the call lands on a neighbouring slot. `IControl` derives a long
   chain (IInspectable -> IFrameworkElement -> IUIElement -> IControl); an off-by-one
   in inherited-slot counting would produce exactly this.
2. `get_Template` legitimately returns null until `OnApplyTemplate` has run, and the
   Button (no Content, no explicit size, no `Canvas.SetLeft/SetTop`) was never
   measured - which would also explain `ActualWidth == 0`. In that case only the
   `ActualWidth` read is meaningful and the conclusion drawn from it was still wrong.

Suspect 1 is worth confirming because a bad slot offset would silently corrupt other
`IControl` calls the host makes. Suspect 2 is benign.

How to settle it
----------------
Read `get_Template` on a control that is known-good and definitely laid out: give a
CheckBox an explicit `Canvas.SetLeft/SetTop` + `put_Width/put_Height`, pump until
`get_ActualHeight() > 0`, then read `get_Template`. Non-null => suspect 2 (probe was
just early/unmeasured). Still null on a visibly-rendered control => suspect 1, a real
projection defect; cross-check the emitted slot index against the winmd for
`Microsoft.UI.Xaml.Controls.IControl`.

Related
-------
- `winui-unpackaged-missing-resources-pri.md`
