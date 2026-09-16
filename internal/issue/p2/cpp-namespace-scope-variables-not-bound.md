# Namespace-scope C++ variables are not bound (std::cout, std::cerr, std::endl)

## Summary

A C++ import binds functions, types and enums but not namespace-scope objects, so the single
most common line in any C++ program - printing through an ostream - cannot be written:

```
probe1.cb(11,4): 'cout' is not a member of namespace 'std'.
```

Found 2026-09-16 dogfooding libtorch (scratch/dogfood/torch/probe1.cb). Every C++ user reaches
for `std::cout` first; having to fall back to `extern int printf(const char*, ...)` is the first
thing a newcomer hits. The same gap covers `std::cerr`, `std::cin`, and every library's
namespace-scope constant (e.g. `torch::kFloat32`, which is a namespace-scope `constexpr`
object, not an enumerator).

## Repro

```c
import cpp { "torch/torch.h", "iostream" };
extern int main()
{
    std.cout << "hi";      // 'cout' is not a member of namespace 'std'.
    return 0;
}
```

Note the diagnostic is correct and clear; the gap is the missing binding, not the message.
Even with the binding, `std.cout << a << b;` needs the chained-shift grammar fix (see
internal/issue/p2/shift-operator-does-not-chain.md) to be usable.

## Root cause

Not investigated. The clang AST-dump driven binder presumably visits FunctionDecl /
CXXRecordDecl / EnumDecl and skips VarDecl at namespace scope. For `std::cout` the binding
would be an `extern` global of the imported class type `std.ostream` with the mangled C++
symbol name - the symbol already lives in libstdc++/libc++, which is linked.

## Fix direction

Bind namespace-scope VarDecls that have external linkage as CFlat globals of the mapped type,
using the C++ mangled name as the link name. `constexpr`/`const` objects with no out-of-line
definition (header-only constants such as `torch::kFloat32`) need the value materialized
locally instead of an extern reference, or they will not link - handle that case explicitly
rather than emitting a dangling extern.

Coverage: a namespace-scope `extern` object plus a header-only `constexpr` object in the
in-repo fixture header (Test/library/cpp_interop*.h), read from Test/test_cpp_interop.cb.

Related (filed the same day by a parallel std-library dogfood session):
internal/issue/p2/cpp-standard-streams-not-usable.md covers the stream CLASSES
(ostringstream/stringstream); this file covers the namespace-scope OBJECTS (cout/cerr/cin) and
header-only constants. Both are needed before `std.cout << x` works.
