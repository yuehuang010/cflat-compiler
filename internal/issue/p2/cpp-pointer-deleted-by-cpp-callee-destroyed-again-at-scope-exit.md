# An owning pointer deleted by a C++ callee is destroyed AGAIN at CFlat scope exit

RULED 2026-09-20 (R3, `internal/plan/cpp-bridge-transparency.md`): `move p` at the call site
transfers ownership into a raw-pointer C++ parameter and consumes the CFlat owner; borrow stays
the default. Import-side sink annotation is a later option. Ready to start.

Found 2026-09-16 by the C++-interop bug bash round 2 (macOS arm64, Release, master 24016619).

## Summary

`new MyImpl(...)` gives CFlat an owning `MyImpl*`. Handing that pointer to a C++ function whose
body does `delete p` (the normal C++ ownership-transfer idiom, and the only one available for a
raw `Iface*` parameter) leaves the CFlat local still marked owning, so the frame runs the deleting
destructor a second time at scope exit: a use-after-free on the vtable slot followed by a double
free. The program completes its asserts, prints, and then segfaults on the way out.

There is no diagnostic, and no spelling that avoids it:

* the CFlat-side equivalent (`delete b;` through the base pointer) IS caught, with a good message -
  so the checker has the information, it just does not see through the C++ call;
* `destroyThrough(move m)` is refused: *"parameter 'p' only uses the argument and does not free it,
  so 'move m' transfers nothing"* - the C++ parameter carries no move annotation;
* `_ = move m;` after the call double-frees immediately (crash before the next statement).

## Repro

`scratch/bb2_pv.h`:

```cpp
#pragma once
namespace bb2pv {
struct Iface { virtual int value() const = 0; virtual ~Iface() {} };
inline int callValue(Iface* p) { return p->value(); }
inline void destroyThrough(Iface* p) { delete p; }
}
```

`scratch/bb2_pv.cb`:

```cflat
import cpp "bb2_pv.h" cache;

int g_dtor = 0;

[cpp] struct MyImpl : bb2pv.Iface
{
    int v = 41;
    MyImpl(int a) { v = a; }
    ~MyImpl() { g_dtor = g_dtor + 1; }
    override int value() { return v + 1; }
};

extern int main()
{
    MyImpl* m = new MyImpl(10);
    bb2pv.Iface* b = m;
    if (bb2pv.callValue(b) != 11) return 101;
    if (b->value() != 11) return 102;
    g_dtor = 0;
    bb2pv.destroyThrough(b);        // delete through the base pointer, C++ side
    if (g_dtor != 1) return 103;
    printf("pv ok\n");
    return 0;
}
```

    x64/Release/cflat scratch/bb2_pv.cb -i scratch -o scratch/bb2_pv.out   # exit 0
    scratch/bb2_pv.out
    pv ok
    Segmentation fault: 11      (run=139)

Every assertion passed - the destructor ran exactly once inside the C++ call. The crash is the
second destruction.

## Root cause

`lldb --batch -o run -o bt scratch/bb2_pv.out`:

```
pv ok
stop reason = EXC_BAD_ACCESS (code=1, address=0x107a)
* frame #0: main + 216
->  0x100001c34 <+216>: ldr    x8, [x8, #0x10]
    0x100001c38 <+220>: blr    x8
```

That is the scope-exit cleanup loading the deleting-destructor slot out of the already-freed
object's vtable. Ownership of `m` was never released: the C++ parameter `Iface* p` is treated as a
borrow, and nothing in the binding says the callee frees it.

Contrast `scratch/bb2_pv2.cb`, identical but with `delete b;` written in CFlat:

    bb2_pv2.cb(15,4): cannot delete 'b' - it copies 'm', which still owns the object and frees it
    at scope exit, so this is a double-free. Delete 'm' instead, or use 'move m' to take ownership
    out of it (which nulls it).

## Fix direction

Two rungs:

1. Give the user an escape hatch: allow `move` into a plain C++ pointer parameter (or a way to
   annotate a bound C++ parameter as consuming), so ownership can be surrendered explicitly. Today
   the `move` is refused by the "parameter only uses the argument" check, which is derived from the
   C++ signature and cannot be right for a `delete`-ing callee.
2. Until then, the failure should not be silent. A pointer that is still owning at scope exit and
   was passed to an imported C++ function is at minimum a case to diagnose.

Suggested bucket: **p2** (silent use-after-free + double free, no diagnostic, no workaround).
