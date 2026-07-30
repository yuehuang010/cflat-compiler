# `new C(...)` inside `[winrt] class C`'s own body skips the vtable + refcount init

Created: 2026-07-14 (found while fixing the `?.` HResult chain leak - the two-link regression
test wanted a `Counter* Clone()` factory on `Counter` itself and crashed)

## Summary

A `[winrt]` class instantiated from **inside one of its own methods** comes out with a null
`lpVtbl` and a zero `__refcount`. The object is otherwise valid (fields are constructed, the
pointer is non-null), so it survives field reads and only dies at the first vtable dispatch -
a call to address 0 - or gets freed early because its refcount starts at 0 instead of 1.

Instantiating the SAME class from a different class's method is fine. Only self-instantiation
is broken, so the common `Clone()` / `Split()` / self-factory shape is unusable today.

## Repro

```cflat
import "com.cb";

[uuid("a1b2c3d4-e5f6-4789-8abc-def012345678")]
interface ICounter { i32 Value(); Counter* Clone(); };

[winrt] class Counter : ICounter
{
    i32 total = default;
    Counter(i32 start) { this.total = start; }
    i32 Value() { return this.total; }
    HResult<Counter*> Clone()
    {
        HResult<Counter*> r = default;
        r.succeed(new Counter(this.total));   // <-- self-new: no vtbl, refcount 0
        return r;
    }
};

extern int main()
{
    Counter* c = new Counter(7);
    HResult<Counter*> cl = c->Clone();
    printf("field=%d\n", cl.value->total);    // 7 - fine, no vtable needed
    HResult<i32> v = cl.value->Value();       // ACCESS_VIOLATION: execute at 0x0
    return 0;
}
```

No `?.` chaining is involved; this is the plain bound-HResult path.

Emitted IR - `new Counter` inside `Maker::Make` (a DIFFERENT class) gets the init:

```llvm
%2 = call ptr @"_operator new_U8Ptr_i64_"(i64 16)
...
store ptr @__winrt_Counter_vtbl, ptr %2, align 8      ; <-- present
%__refcount = getelementptr inbounds %Counter, ptr %2, i64 0, i32 1
store i32 1, ptr %__refcount, align 4                 ; <-- present
```

while `new Counter` inside `Counter::Clone` (its OWN method) gets neither store - the object is
handed out with whatever field 0 the by-value constructor result left there (null) and refcount 0.

## Root cause

`MainListener.h` (`isWinrtNew`, ~line 11256) gates the vtable+refcount post-init on
`compiler->IsWinrtClass(typeName)`, i.e. on `winrtClasses` containing the class.

`winrtClasses[className]` is only populated at the very END of `LLVMBackend::EmitWinrtRuntime`
(`LLVMBackend.h`, ~line 8165), and that function - by its own contract - "must run AFTER the object
struct body, the vtable struct, and **the user member functions** exist". So while the class's own
method bodies are being codegen'd, its `winrtClasses` entry does not exist yet, `IsWinrtClass`
returns false, and the `new` lowers as a plain (non-COM) `new`.

It works from another class purely because classes are emitted in source order: by the time
`Maker`'s bodies are walked, `EmitWinrtRuntime(Counter)` has already run. (Which also means a
`[winrt]` class that `new`s a `[winrt]` class defined LATER in the file is likely broken the same
way - untested.)

## Fix direction

Decouple "is this a `[winrt]` class" and "where is its vtable global" from "has its runtime been
emitted":

1. Gate `isWinrtNew` on the forward-scanned `[winrt]` annotation (which already resolves before
   definition - `testAnnotationBeforeDef` in `Test/test_com.cb` proves `annotationof` does), not on
   `winrtClasses` membership.
2. Create the vtable `GlobalVariable` (`__winrt_<Class>_vtbl`) EARLY - at class registration, when
   the vtable struct type is known - with no initializer, and have `EmitWinrtRuntime` fill in the
   initializer later via `setInitializer`. Today it is `new llvm::GlobalVariable(..., init, ...)` at
   emit time, so a `new` that runs earlier has nothing to store; creating a second global from the
   `new` site would get a `.1` suffix and silently not alias the real vtable.

Both halves are needed: (1) alone would store a garbage/undefined vtable pointer.

## Workaround until fixed

Put the factory on a *different* class that is defined earlier in the file (this is what
`Test/test_com.cb` does - `Maker::Make` produces a `Counter`, and `Counter::Adopt` produces a `Cat`,
both defined above their producer).
