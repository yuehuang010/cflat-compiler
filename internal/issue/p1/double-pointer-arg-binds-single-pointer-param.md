# A `T**` argument binds a `T*` parameter and silently reads the wrong bytes

Found while fixing `pointer-arg-binds-by-value-class-param.md` (the `T*`-into-by-value-`T` hole).
Same predicate, different axis: the scorer's pointer comparison is a BOOLEAN (`Pointer`), so it
answers "is it a pointer" and never "how many stars".

Repro (`scratch/p/ptrarg_a3_ptrptr_into_ptr.cb`):

```cflat
class Circle { int r = 0; };
int byPtr(Circle* c) { return 2000 + c->r; }

extern int main()
{
    Circle* a = new Circle(); a->r = 3;
    Circle** pp = &a;
    return byPtr(pp);           // a Circle**, not a Circle*
}
```

Measured on the Release binary at f45c9ad and again after the `IsTypeMatch` pointer gate landed:
compiles clean, links, RUNS, and exits with a garbage value. `2003` is the only correct answer
(exit code 211) and no build gives it. **The exit code is ENVIRONMENT-dependent, not build-dependent
- do not read a change in it as a change in behaviour, and never pin one in a test.** The same
post-fix binary gives rc 176 with the executable written into one directory and rc 224 with it
written into another; values of 16, 80, 208 and 112 were seen along the way. That is `c->r` reading
the low bytes of the heap address stored in `a`, so the number tracks the process image. No
verifier complaint - under opaque pointers `ptr` and `ptr` are the same LLVM type, so unlike the
`T*`-into-`T` case there is nothing for the module verifier to catch. Silent wrong value.

Root cause, established by elimination: nothing else in `ComputeOverloadFunction` accepts this
argument, so the call compiling at all proves `TypeAndValue::IsTypeMatch` (cflat/LLVMBackend.h)
returned true - i.e. it now gates on `Pointer` but not on indirection DEPTH. `TypeAndValue` records
depth only as `ElemPointer` (a `T**` bit); the general `PointerDepth` int lives on the
function-pointer signature components (`FuncPtrParam`), not on the value type. Whether the argument
actually carries `ElemPointer=true` here is UNVERIFIED and is the first thing to measure.

Fix direction: gate `IsTypeMatch` on `ElemPointer` the way it is now gated on `Pointer`. Held out
of the `T*`-into-`T` fix deliberately - that fix rejects a shape NO compiling program can contain
(a by-value struct parameter is an LLVM struct slot at every size, so the shape always failed
verification), whereas an `ElemPointer` rejection would reject programs that compile and run
today. `ElemPointer`'s population is also not uniform across producers (C interop, WinRT and
synthesized signatures set only `Pointer`), so the gate must be ONE-SIDED - reject only a proven
`T**` argument at a proven `T*` parameter - and it needs its own accept-set and differential sweep
before it lands.
