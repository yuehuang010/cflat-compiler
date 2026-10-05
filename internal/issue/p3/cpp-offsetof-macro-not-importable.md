# offsetof(S, member) is not available after import cpp "cstddef" / "stddef.h"

`offsetof` is a function-like macro whose first argument is a type, so it never becomes a CFlat callable.
Not listed by `-v` as rejected either.

## Repro

```cflat
import cpp "cstddef";
struct Layout { char tag = default; double value = default; };
extern int main() { i64 o = offsetof(Layout, value); return (int)o; }   // expected 8
```

Actual: `Undefined variable offsetof.` Case: test_libs/std_full/std_full_11_923_offsetof_macro.cb.

## Root cause (GUESS)

The macro expands to `__builtin_offsetof(type, member)`, a type argument the function-like macro
translation cannot express.

## Fix direction

Recognise `offsetof` as a compiler intrinsic (`offsetof(T, field)` over CFlat structs), or add it to the
CFlat language surface.

Found by: test_libs/std_full/std_full_11_language_support (2026-10-02).

## Status after T23 round 2 (2026-10-03)

Function-like system macros now bind at their call through a generated C++ wrapper
(`RequestCxxFreeFunction`, `assert(cond)` works). `offsetof` reaches that path and fails with
`C++ macro 'offsetof' cannot be called with these arguments (clang: too few arguments provided to
function-like macro invocation)`: its first argument is a TYPE, and `Layout` is a plain CFlat struct,
which can never be spelled in C++ (bridge ruling R2). So this cannot come from the C++ side; it needs
a CFlat intrinsic `offsetof(T, field)` over CFlat struct layouts (a language-surface addition -
maintainer ruling on the spelling first; `sizeof`/`alignof` handling in the backend is the model).
