# Interface rebox is emitted before the implementor registry is complete

Filed 2026-07-26, split out of the fix for
`iface-upcast-to-non-implemented-interface-segfault.md` (branch
fix/iface-upcast-non-implemented). The STATIC check landed there is now
declaration-order independent, but the CODEGEN underneath it is not: an
interface -> interface rebox is lowered at the conversion site, using whatever
implementors happen to be registered at that moment.

## Repro 1 - derived-to-parent upcast above the implementor

```cflat
import "memory.cb";
interface IElement { int area(); };
interface IButton : IElement { int poke(); };
class EarlyBtn : IButton { int r = 2; EarlyBtn() {} int area() { return r * r; } int poke() { return 1; } };
IElement widen(IButton b) { IElement e = b; return e; }
class LateBtn : IButton { int r = 4; LateBtn() {} int area() { return r * r; } int poke() { return 7; } };
extern int main() { IButton g = new LateBtn(); IElement e = widen(g); return e.area() - 16; }
```

Exit 139. `widen`'s body is emitted before `LateBtn` exists, so the typedesc
if-chain only tests `EarlyBtn`; at runtime the value is a `LateBtn`, nothing
matches, and the zeroed fat pointer is dispatched through. The same shape with
a single implementor declared after the helper (`scratch/x_earlyhelper_sameclass.cb`)
emits an EMPTY chain and fails identically. A generic implementor
(`class Box<T> : IA, IB` monomorphized in `main`) fails the same way, since the
instance does not exist until the instantiation site is reached.

This is the pattern `example/ui/*` is built on, so it is not a corner case.

## Repro 2 - core-library rebox sites

`core/ui_native.cb` and friends are code-gen'd during `ProcessImports`
(driver: `LLVMBackend.cpp` ~653-780), which runs before the user's file is even
forward-ref scanned. A rebox site inside a core library therefore can never see
a user class that implements the interface, no matter what the user writes.

## Root cause

`RebuildInterfaceFatValue` (`cflat/LLVMBackend.h` ~9903) lowers the conversion
eagerly: it walks `dataStructures` / `programTable` for implementors of the
destination interface that already have a `typeDescriptor` global and emits one
compare-and-store per match. Everything declared later - later in the file,
later as a generic instantiation, or in the importing file when the site is in a
core library - is simply absent from the chain.

The registry cannot just be widened to include forward-declared classes: a class
the ForwardRefScanner has seen still has an OPAQUE `StructType` with no fields, so
`GetOrCreateVTable` on it would cache a vtable in `sd.VTables` with null
field-offset slots (`AppendInterfaceFieldOffsetSlots` gets `layout == nullptr`)
and a full destructor synthesized from an empty `StructFields`. That trades a
crash for a silent miscompile. The emission has to move, not the registry.

## Fix direction

Defer rebox emission to finalization, mirroring the existing deferred-destructor
pattern: `GetFullDestructorForDelete` hands out an empty `.dtordeferred` and
`EmitDeferredFullDestructorBodies` (`LLVMBackend.h` ~4477) fills the bodies at
finalization, called from `Compile` (`LLVMBackend.cpp` ~794) and `Analyze`
(~2333) but deliberately NOT from `CompileImportedFile`.

Sketch:

1. `RebuildInterfaceFatValue` emits a call to a get-or-created internal
   `__iface_rebox.<dstIface>` function instead of an inline if-chain, and records
   `(srcIface, dstIface, source location)` for the site.
2. A new `EmitDeferredInterfaceReboxBodies()` runs at finalization, when every
   `StructType` body, destructor, and generic instantiation exists, and emits the
   real typedesc chains.
3. The static impossibility check and the zero-cases error move there too, and
   report against the recorded locations.

Known risks to plan for:

- Insert-point management. The rebox body uses the member `builder` helpers
  (`CreateBasicBlock`, `SwitchToBlock`), unlike the destructor wrappers, which
  build on a local `IRBuilder`. The finalization pass must save and restore the
  member builder state around each body.
- Diagnostics need the recorded location; `LogError` at finalization has no
  current parse context.
- Finalize-time errors fire after every scope has closed, so a regression test
  for them needs the bare-semicolon file-scope form of `expect_error`, not the
  scoped-block form.

## Current mitigation

`InterfaceConversionIsProvablyImpossible` / the `emittedCases == 0` backstop only
report when impossibility is PROVABLE from the scanner-wide registry, and stay
silent whenever an implementor may merely not be registered yet. So none of the
programs above is falsely rejected; they compile and reproduce the runtime
behaviour described here.

One consequence of that conservatism: an interface named in the base clause of a
class inside an `if const` block is marked uncertain even when the branch is not
taken on this platform, so the static check goes silent for that interface and the
null-vtable crash is not caught. Deferring emission to finalization removes the
need for the uncertainty marking and closes this too.
