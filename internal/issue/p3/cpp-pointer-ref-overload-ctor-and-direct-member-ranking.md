# C++ `const char *const &` overloads: constructor wrapper and direct non-template ranking gaps

Found by the t28 round-5 matrix (literal / `const char *` vs a `const char *const &` candidate).
Every cell below is the same on master; none goes through the free / function-template wrapper
paths t28 fixed (those match clang in every cell).

## Repro

Header `h.hpp`:

```cpp
#include <cstring>
#include <string_view>
namespace m5 {
inline int chosen = 0;
__attribute__((noinline)) inline int scribble() { volatile char b[2048]; for (int i = 0; i < 2048; ++i) b[i] = 0x5a; return b[0]; }
__attribute__((noinline)) inline int consume(const char* const* p, int) { return (int)std::strlen(*p); }
struct C { const char* const* q; C(const char* const& p) : q(&p) { chosen = 1; }
           template<class T> C(const T&) { chosen = 2; static const char* z = "compete"; q = &z; } };
struct S { const char* const* m(const char* const& p) { chosen = 1; return &p; }
           const char* const* m(std::string_view) { chosen = 2; static const char* z = "compete"; return &z; } };
const char* const* f(const char* const& p) { chosen = 1; return &p; }
const char* const* f(std::string_view) { chosen = 2; static const char* z = "compete"; return &z; }
}
```

```cflat
import cpp "h.hpp";
extern int main()
{
    const char* n = "abcde";
    printf("%d\n", m5.consume(m5.C(n).q, m5.scribble()));     // clang 5 (chosen 1), CFlat 7 (chosen 2)
    printf("%d\n", m5.consume(m5.C("abc").q, m5.scribble())); // clang 3, CFlat SIGSEGV
    m5.S s = m5.S();
    printf("%d\n", m5.consume(s.m("abc"), m5.scribble()));    // clang 3 (chosen 1), CFlat 7 (chosen 2)
    printf("%d\n", m5.consume(m5.f(n), m5.scribble()));       // clang 5 (chosen 1), CFlat 7 (chosen 2)
    return 0;
}
```

Also: a non-template `const char (&)[4]` or by-value `const char *` sibling of a non-template
`const char *const &` is ambiguous in clang for a literal, but CFlat's direct resolution accepts
one (free and member). Full matrix: t28 worktree `scratch/t28r5/mx` (`gen.py`, `oracle.sh`, `run.sh`).

## Root cause (not yet verified)

- Constructor: `__cflat_ctor_` wrappers (LLVMBackend_CInterop.cpp, mirror selection) rank a mixed
  non-template / template constructor set differently from clang, and a literal reaching a
  `const char *const &` constructor parameter binds a decayed pointer temporary in the wrapper frame
  (same defect t28 fixed for free / template calls: RetargetCxxLiteralPointerTemporaries).
- Direct binding: CFlat's own overload ranking prefers a converting class parameter
  (`std::string_view`, `const std::string &`) over an exact `const char *const &`, and has no
  ambiguity for the array / by-value pointer ties.

## Fix direction

Constructor: reuse the extractor's `calleeIdentity` / literal pointer temporary report and re-spell
the constructor wrapper's parameter as `const char *const &` (caller slot). Direct binding: rank
`X *const &` from an `X *` / literal as an exact match ahead of user-defined conversions; refuse the
array-reference / by-value-pointer ties as clang does.

## Also (main-session review of t28 round 5, 2026-10-05)

- A literal at a template `const char *&&` parameter that keeps the address
  (`template<class U> const char* const* rr(const char*&& p, const U&) { return &p; }`,
  `consume(rr("abc", 1), scribble())`): clang accepts and prints 3. Master SIGSEGVs; t28 refuses
  ("... cannot keep that pointer alive in the calling frame for the same overload"), because the
  caller-slot re-spelling is `X *const &` only. Safe, but a refusal clang does not make.
- Free path: the caller-slot re-spelling is skipped when the operator scalar retarget already
  fired (`retarget.empty()` guard in RequestCxxFreeFunction); not probed.
