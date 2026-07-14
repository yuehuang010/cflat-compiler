# `concretePtr as IFace` compiles to a failing downcast, then crashes

Status: OPEN. Found 2026-07-14 while validating the fix for the Lambda-field owned-string
leak (that fix is unrelated - this reproduces on any class/interface pair).

## Summary

Using `as` to box a CONCRETE class pointer into an interface value is lowered as a
runtime-checked DOWNCAST rather than a box. The check always fails, the resulting fat
pointer is `{ null, null }`, and the first method call through it dispatches to address 0
(access violation). No diagnostic is emitted at compile time.

The supported spelling - plain assignment `IW e = w;` - boxes correctly. `as` is correct
in the other direction (`W* back = e as W;` on an interface value works).

## Repro

```cflat
interface IW { int id(); };
class W : IW { int n = default; int id() { return 8; } };

extern int main()
{
    W* w = new W();
    IW e = w as IW;              // `IW e = w;` is the working form
    printf("id=%d\n", e.id());   // access violation (0xC0000005)
    return 0;
}
```

## Root cause

The `as` lowering emits the interface-source downcast sequence for a concrete-pointer
source: it loads the object's first word as a vtable pointer, loads `vtable[0]` as the
typedesc, and compares it to `@W_typedesc`. A concrete object has no vtable/typedesc
header (`%W = type { i32 }`), so the compared word is just the first FIELD. The check
never matches, the `select` yields a null vtable, and the subsequent
`getelementptr ptr, null, 1` + `load` + indirect call faults.

Because the source is a plain `T*` and the destination is an interface, this should either
box (identical to the assignment path) or be rejected.

## Fix direction

In the cast path, when the SOURCE is a concrete struct/class pointer (not an interface fat
pointer) and the DESTINATION is an interface the source implements, emit the box
(`BuildInterfaceFatValue` with the class's vtable) exactly as the assignment path does.
When the source does not implement the interface, `LogError` instead of emitting the
typedesc check. Reserve the runtime-checked downcast for an interface-typed source.
