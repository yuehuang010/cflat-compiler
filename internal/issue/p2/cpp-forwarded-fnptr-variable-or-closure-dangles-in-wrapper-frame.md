# A function-pointer variable or closure forwarded to a C++ `T&&` dangles in the wrapper frame

Sibling of the fixed literal case (t28: `std.forward_as_tuple("abc")`, and a function NAME, which now
crosses as the function lvalue). Still WRONG CODE when the forwarded reference escapes through the
result (forward_as_tuple-shaped templates, std::forward_as_tuple):

## Repro

```cflat
import cpp "library/cpp_t28_fwdref.hpp";   // Test/library fixture
extern int main()
{
    int k = 4;
    Lambda<int(int)> f = (int v) => v * k;
    printf("a %d\n", std.cflat_t28.call5(std.cflat_t28.fwd(f)));   // clang 20, CFlat 5
    function<int(int)> g = (int v) => v + 1;
    printf("b %d\n", std.cflat_t28.call5(std.cflat_t28.fwd(g)));   // clang 6, CFlat 5
    return 0;
}
```

(Compile with `-i Test`.) Same through the template path (`t28.fwd1(g)` + `t28.call5`).

## Root cause

- `function<>` variable: both wrapper spelling sites (RequestCxxFunctionTemplate and
  RequestCxxFreeFunction in LLVMBackend_CInterop.cpp) pass a thin function pointer BY VALUE and
  forward `static_cast<F &&>(pN)`, so `T&&` binds the wrapper's own parameter. C++ deduces
  `R(*&)(A)` against the caller's variable, but the binder refuses a reference-to-function-pointer
  parameter ("parameter ... is a reference to a function pointer"), so the caller slot cannot be
  handed over as `F &`.
- Closure (`Lambda<>` / capturing): CxxClosureCallArgument builds the owning `__cflat_closure<>`
  prvalue INSIDE the wrapper; the forwarded `closure&&` refers to that wrapper temporary.

## Fix direction

Function pointer: pass the caller slot's address (a `void *` or `F *` wrapper parameter, the call
spelled `*static_cast<F *>(pN)` for a proven lvalue, a caller-frame temp for an rvalue), or teach the
binder `R (*&)(A)` / `R (*&&)(A)` via LowerAliasByPointerArg. Closure: needs the closure object in a
caller-frame slot (C++ type), which no current path materializes. Check every cell against
`clang++ -std=c++20`.

Found by: t28 sibling audit (2026-10-03).
