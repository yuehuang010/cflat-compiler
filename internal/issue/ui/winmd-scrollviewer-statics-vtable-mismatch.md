# WinMD projection: IScrollViewerStatics calls crash (vtable slot mismatch)

## Summary

Calling any method on the projected `Microsoft.UI.Xaml.Controls.IScrollViewerStatics` crashes.
The projected vtable appears to disagree with the runtime's actual layout, so the call lands on the
wrong slot. Calling the raw slot at the metadata-derived index crashes too, which rules out a simple
off-by-one in the generated wrapper and points at the metadata -> slot-index mapping itself.

This is the same family as `winui-icontrol-get-template-misreads.md` (a projected interface whose
slots do not line up with the runtime), so the two are probably one root cause.

## Repro

In `cflat/core/ui_native/winui.cb`, attach a ScrollViewer attached-property setter through the statics:

```cflat
XamlScrollViewerStatics* svs = nullptr;
activationFactory("Microsoft.UI.Xaml.Controls.ScrollViewer", iidof(XamlScrollViewerStatics), (void**)&svs);
svs->lpVtbl->SetCanContentRenderOutsideBounds(svs, elem, false);   // crashes
```

Discovered 2026-07-13 while binding real items into the WinUI ListView/TreeView: with rows finally
present, the items controls rendered their cached (off-viewport) rows over neighbouring cards, and
`CanContentRenderOutsideBounds` is the documented XAML lever for that.

## Root cause

Not diagnosed. `IScrollViewerStatics` is a statics interface with a long run of attached-property
get/set pairs; if the projection derives its slot index from a different ordering than the runtime
vtable uses (e.g. metadata declaration order vs. the runtime's interface-inheritance-flattened
order), every call after the divergence point is dispatched to the wrong function pointer.

Worth checking whether the projection accounts for the `IInspectable` prefix slots and for methods
inherited from a required interface, and whether statics interfaces take a different path from
instance interfaces in `WinmdSignature.cpp` / `EmitWinrtThinSlotCall`.

## Current workaround (in tree)

The WinUI host does not call the statics at all. The overdraw is fixed by zeroing the items panel's
`CacheLength` instead, which stops XAML realizing the off-viewport rows in the first place. That is a
legitimate fix for the overdraw, but it leaves the projection bug live for any other caller.

Note `UIElement.Clip` does NOT contain the overdraw here either - the WinUI items template lets its
inner ScrollViewer render outside the parent's bounds - so `CacheLength` is currently the only lever.

## Fix direction

Find the slot-index divergence for statics interfaces, then re-test with the direct call above. Once
it works, the `CacheLength` workaround can stay (it is cheaper) but the projection must stop crashing.
