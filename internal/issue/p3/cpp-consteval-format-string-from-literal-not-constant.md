# A string literal passed to a consteval-constructed parameter is not a constant expression

`fmt::format("{}-{}", 7, 3.5)` fails:

```
no instantiation of C++ function template 'fmt.format' accepts these argument types (char*, int, double)
(clang: call to consteval function 'fmt::fstring<int &, double>::fstring<const char *, 0>' is not a constant expression)
```

The format-string parameter is a class with a `consteval` converting constructor (C++20
compile-time format checking - fmt, std::format). The generated template-instantiation wrapper
receives the literal as a runtime `const char *` parameter, so clang cannot evaluate the consteval
constructor. `fmt.format(fmt.runtime("{}-{}"), 7, 3.5)` works. Found 2026-09-26 probing fmt 12.2
(`scratch/ladder/fmt/f2.cb` has never passed).

## Repro (standalone, ~1 s)

```cpp
// ce2.h
#pragma once
#include <type_traits>
namespace ce2 {
template <class... A> struct fstr {
    const char* p;
    template <class S> consteval fstr(const S& s) : p(s) {}
};
template <class... A> using fs = fstr<std::type_identity_t<A>...>;
template <class... A> int count(fs<A...> f, A&&... a)
{ int n = 0; for (const char* q = f.p; *q; ++q) if (*q == '{') ++n; return n; }
}
```

```cflat
import cpp "ce2.h";
extern int main()
{
    return ce2.count("{{}}-{{}}", 7, 3.5) == 2 ? 0 : 1;   // "{}-{}" after CFlat brace escaping
}
```

Fails with `call to consteval function 'ce2::fstr<int, double>::fstr<const char *>' is not a
constant expression`. Control that works: a NON-template function taking a class with a plain
`consteval CS(const char*)` constructor (`ce.len("abc")`).

## Fix direction

When a CFlat string literal (not interpolated) is the argument for a parameter whose type has a
consteval constructor, spell the LITERAL itself into the generated wrapper source at that argument
position instead of forwarding a `const char *` parameter (the literal is known at compile time).
Watch escaping: CFlat `{{` is a literal `{`, so emit the unescaped text as a proper C++ string literal.

## Notes for the fixing agent

- Regression: extend an existing `Test/library/cpp_interop_*.h` fixture with the `ce2.h` shape and
  a leg in `Test/test_cpp_interop.cb`. Add an `expect_error` case for an interpolated string or
  `const char*` variable into the same parameter (must stay a clear error). No new test files.
- Do NOT add fmt to the test run (maintainer, 2026-09-26). Local check only:
  `bash scratch/probe3/run.sh` (f02) - fmt from Homebrew, `/opt/homebrew/opt/fmt`.
