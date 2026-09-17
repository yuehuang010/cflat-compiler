# Binding a C++ reference RETURN to a pointer local is rejected by a message that contradicts itself

Found 2026-09-16 by the C++-interop bug bash round 3 (macOS arm64, Release, worktree at master 2798eb1a).

## Summary

A C++ function returning `C&` / `const C&` is documented (Test section M19) as binding "as a
pointer". Initializing a CFlat pointer local from such a call is nevertheless refused, and the
message states the right-hand side's type IS the pointer type it says is missing:

```
bb3_const2.cb(8,17): cannot initialize pointer 'mr' with a value of type 'bb3c.C*' - the
right-hand side must be a pointer (call getPtr() or use '&')
```

Writing `&` in front works and aliases correctly, so this is a usability/diagnostic defect, not a
capability gap. Both the const and non-const reference returns behave the same way.

## Repro

`scratch/bb3_const.h`:

```cpp
namespace bb3c {
struct C { int v; C(int x) : v(x) {} int get() { return 1; } int get() const { return 2; } };
inline C& mut_ref() { static C c(11); return c; }
inline const C& const_ref() { static C c(9); return c; }
}
```

```cflat
import cpp "bb3_const.h";
extern int main()
{
    bb3c.C* mr = bb3c.mut_ref();       // compile exit 1, message above
    return mr.v;
}
```

Working form (compile 0, run 0) - `scratch/bb3_refret.cb`:

```cflat
bb3c.C* mr = &bb3c.mut_ref();
printf("v=%d get=%d\n", mr.v, mr.get());   // v=11 get=1
mr.v = 31;
printf("second call v=%d\n", bb3c.mut_ref().v);   // 31 - the alias is real
```

## Fix direction

Either accept the initialization directly (the call's result already IS the reference's address, as
the `&` form proves), or keep rejecting it and fix the message so it does not print the pointer type
as the reason the value is not a pointer - e.g. "a C++ reference return is an lvalue; write
`&<call>` to take its address".

Suggested bucket: p3 (diagnostic quality; workaround is one character).
