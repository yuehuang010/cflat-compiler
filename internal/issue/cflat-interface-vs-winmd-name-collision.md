# A cflat interface whose name collides with an imported WinMD type emits INVALID IR (no diagnostic)

Created: 2026-07-13 (found during Phase 3 of the UI interface refactor)

## Summary

If a cflat `interface` has the same name as a type projected from an imported
`.winmd`, the WinMD type wins the type lookup. A cflat interface VALUE (a fat
pointer) is then treated as a COM object pointer and GEP'd as the WinRT struct.
The result is **invalid LLVM IR**, not a compiler error:

```
Invalid bitcast
  %3 = bitcast ptr %2 to %__iface_fat_ptr
GEP base pointer is not a vector or a vector of pointers
  %4 = getelementptr inbounds %IButton, %__iface_fat_ptr %3, i32 0, i32 0
```

Per the repo convention (CLAUDE.md): when a bad-IR / LLVM-assert path is
reachable from ordinary source, the compiler must produce a proper error instead.

## Repro

`cflat/core/ui_native/winui.cb` imports the WinUI winmd, which projects WinRT
`IButton` (used at `winui.cb:117`) and `IImage` (`:587-589`: `iidof(IImage)` plus
a `put_Source` vtable call). Declaring

```cflat
interface IButton : IDisableable { string title; };
```

in `core/ui_native.cb` and boxing a `Button` into it produces the IR above.

Note the collision only bites when a file in the IMPORT CLOSURE actually NAMES
the WinMD type. `IListView`, `ISlider`, `IComboBox` all exist in the WinUI winmd
too, but `winui.cb` never names them as WinRT types, so those resolve to the
cflat interface and work fine. The rule is:

> A cflat interface name is unusable if any file in the import closure names a
> WinMD type of the same name.

Fully-qualified WinMD spellings do not dodge it: `iidof` rejects them
(`no IID known for type 'Microsoft.UI.Xaml.Controls.IImage'`).

## Impact

Phase 3 of the UI refactor had to name its interfaces `IButtonElement` and
`IImageElement` instead of the natural `IButton` / `IImage`
(`internal/plan/ui-interface-refactor.md`). Every other widget interface kept
its natural name.

## Fix direction

1. **Diagnose it.** At interface registration (or at the boxing site), detect
   that the resolved type name refers to a WinMD projection rather than the
   declared cflat interface and `LogError` with both origins named. Anything is
   better than invalid IR.
2. **Then consider resolving it properly.** Options, roughly in cost order:
   - Let a locally declared `interface` SHADOW a WinMD projection of the same
     name, and require the WinMD spelling to be disambiguated at its use site.
   - Namespace-qualify WinMD projections so they never occupy bare identifiers.

Note `winui.cb:117` uses WinRT `IButton` only as an arbitrary IUnknown stand-in
for a slot-0 QI (its own comment says so); switching it to `IButtonBase` would
free the `IButton` name for cflat. `IImage` at `:587-589` is a real, load-bearing
use and cannot be freed that way.
