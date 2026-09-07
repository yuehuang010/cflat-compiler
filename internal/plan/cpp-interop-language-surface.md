# C++ interop - language surface (supplemental)

Status: proposed. Supplements `cpp-direct-abi-interop.md`, which covers the ABI/Clang
side and says nothing about what a CFlat programmer writes. This document owns the
user-facing design: import spelling, name lookup, types, calls, lifetime, and the
reconciliation with existing CFlat rulings. Every "RULING" line needs a maintainer
decision before the milestone that depends on it starts.

No compiler changes are part of this document.

## Why direct ABI (the decision the base plan asserts)

| Approach | Used by | Cost | What the user sees |
|----------|---------|------|--------------------|
| Hand-written C shim | Rust `bindgen` + manual, Zig, Go cgo | Low compiler cost, high user cost | Writes and maintains a shim per API |
| Generated extern-C wrappers | Rust `cxx`/`autocxx`, SWIG, D `dpp` | Medium | Declares what to bridge; templates need explicit instantiation lists; extra call layer; a `.cpp` is compiled per import |
| Direct ABI via Clang | Swift ClangImporter (shipped), Carbon (experimental), Google Crubit (experimental) | High compiler cost, near-zero user cost | Header in, calls out; inline/template code emitted on demand |

Direct ABI is chosen because CFlat's value proposition is "extended C with no binding
layer", and the C side already delivers that (auto-extern from a `.c`, header binding
with no prototypes). A wrapper layer would be the first place a CFlat user writes
bridging code by hand or by declaration list. Swift proves the direct route works and
also proves its cost: it needed private Clang CodeGen access. That cost is the M0 gate
in the base plan; if M0 fails, the fallback is generated wrappers behind the SAME
surface described here, not a different surface. The surface must therefore not leak
which strategy is underneath.

## Import spelling

The `cpp` keyword goes in front of the path and selects C++ mode. Extension never selects
the mode: `.h` under `import cpp` is C++, `.hpp` under plain `import` is the existing C
header path. Ruled 2026-09-06.

```c
import cpp "widgets.h";                       // C++ header, any extension
import cpp "impl.cpp";                        // compiled by clang++ and linked, like .c today
import cpp package "fmt/core.h" lib "fmt.lib"; // prebuilt C++ library
import cpp { "a.hpp", "b.hpp" };              // grouped, one translation unit, one macro state
import cpp "big.hpp" cache;                   // existing cache clause applies unchanged
```

- `cpp` sits right after `import`, before `package` / `program` / `{`, and composes with
  every trailing clause (`lib`, `define`, `cache`, `framework`, `as`).
- A CFlat file may mix `import "x.h"` (C) and `import cpp "y.h"` (C++); they are separate
  sessions and a name declared in both is an error at the use site, not at import.
- `extern "C"` functions inside a C++ header keep C linkage and land in the same
  registration path as today's C import (base plan M2).
- Toolchain profile (`-std`, sysroot, stdlib, defines) comes from CLI flags
  `--cpp-std`, `--cpp-stdlib`, plus the existing `--c-include` / `--c-define`. Defaults
  per host: MSVC on Windows, libc++ on macOS, libstdc++ on Linux. RULING: whether a
  per-import `std "c++20"` clause is wanted or CLI-only is enough for v1.

## Prototype scope (ruled 2026-09-06)

The prototype covers primitives and bare pointers only: scalar and enum arguments and
results, `T*` in and out, `noexcept` free functions in namespaces, `extern "C"` inside C++
headers. Everything else is rejected with a precise LogError at the use site: records by
value, references beyond the pointer-shaped mapping already landed, classes, templates,
operators, virtual dispatch, exceptions. The base plan's M3+ resume from this boundary.

## Name lookup

C++ `a::b::c` is spelled `a.b.c`. CFlat namespaces already use dots, and foreign
namespaces are registered as ordinary CFlat namespaces holding foreign members.

```c
import "geom.hpp";              // namespace geom { struct Vec2 {...}; Vec2 add(Vec2, Vec2); }

geom.Vec2 v = geom.add(a, b);   // qualified, like MathUtils.add today
using geom;                     // existing using form brings the namespace in
Vec2 w = add(a, b);
```

- Global-namespace C++ names join file scope exactly as C header names do today.
- A foreign name and a CFlat name that collide at the same scope: error at use with
  both origins named (same shape as the existing extern-conflict diagnostic). `import
  "x.hpp" as X;` roots the whole header under `X.` to break collisions.
- Nested types and static members are members of the class name: `Outer.Inner`,
  `Widget.count()`. Scoped enums: `Color.Red`; unscoped enums keep C behaviour.
- Overload sets are foreign overload sets. Clang ranks them (base plan contract 3).
  A CFlat function and a foreign function never form one overload set; if both are
  visible for a name, the call is ambiguous and reported. RULING: confirm no mixing.

## Types

| C++ | CFlat spelling | Notes |
|-----|----------------|-------|
| `int`, `long long`, `bool`, `char` | `int`, `i64`, `bool`, `char` | Width by target; `long` maps per profile |
| `T*` | `T*` | Borrow by default, as today |
| `const T*` | `T*` | const dropped per ruling (see Reconciliation) |
| `T&` param | `T` lvalue arg, passed by address | No new syntax |
| `const T&` param | any `T` arg, temporaries allowed | |
| `T&&` param | `move x` arg required | |
| `T&` return | `alias T` | Existing borrow keyword; caller never frees |
| `T&` field or local | not supported v1 | Explicit error |
| `class C` | `C` (a foreign struct type) | Layout from Clang, opaque to `sizeof` rules of CFlat structs |
| `template<class T> class V` | `V<T>` | CFlat generic angle syntax |
| `template<int N>` | `V<T, N>` | Existing value generic parameters |
| `std::string` | `std.string` | Distinct from CFlat `string`; explicit conversions only |
| `enum class E` | `E` | Scoped; `E.A` |
| `void(*)(int)` | existing function pointer type | |
| member pointers | not supported until M6 says so | |

Template arguments accept CFlat primitives, pointers to foreign or CFlat structs, and
foreign types. A CFlat struct as a by-value template argument is accepted only if it
is trivially copyable in CFlat terms (no destructor, no `unique` field); otherwise a
precise error. CFlat generic mangling and C++ template identity never meet: a CFlat
generic `list<geom.Vec2>` is a CFlat instantiation whose element is a foreign type.

## Construction, calls, and members

```c
import "widgets.hpp" cpp;

int main()
{
    ui.Widget w = ui.Widget("title", 3);      // stack object, ctor overload picked by Clang
    ui.Widget d = default;                    // default ctor; error if deleted
    w.resize(10, 20);                         // member call; virtual dispatch is automatic
    int n = w.count();                        // const member
    ui.Widget* p = new ui.Widget("heap");     // operator new + ctor
    unique ui.Widget* q = new ui.Widget("x"); // owned; dtor + operator delete at scope exit
    delete p;                                 // operator delete, matching alignment
    return 0;                                 // w destroyed here, once
}
```

- `T(args)` is the construction expression. `= default` calls the default constructor
  rather than zero-filling; a foreign type with no accessible default ctor rejects it.
- Brace-init on a foreign aggregate uses C++ aggregate rules when the class is an
  aggregate, else it is an error pointing at the ctor overloads.
- `new`/`delete` on foreign types route to the C++ operators (class-specific first),
  never to CFlat's allocator. `unique T*` composes unchanged.
- Operators: a C++ `operator+` etc. binds to CFlat's operator overloading table for
  foreign operand types. `operator[]`, `operator()`, `operator->`, conversions: M5.
- Static data members and header constants are readable; writable if non-const.
- Access control is enforced from Clang: private members are diagnosed as private, not
  as missing.

## Lifetime and ownership - reconciliation with existing rulings

Foreign classes with ctor/dtor are owning types under the "ownership at function
boundaries" rule; nothing new is tracked. The points below are where CFlat and C++
disagree and a rule must be written down.

1. **`=` is total.** For a copyable foreign type, `dest = src` invokes the C++ copy
   assignment (or copy ctor at declaration). For a move-only foreign type
   (`std.unique_ptr`) plain `=` is an implicit consume, which the global/static ruling
   already makes an error in general; `dest = move src;` is required. Same rule as a
   CFlat `unique` value. No annotation on the user side.
2. **`move x` nulls the source.** C++ has no null state; a moved-from object is valid and
   still needs its destructor. Rule: after `move x`, `x` is compile-time consumed
   (existing use-after-move diagnostics apply) and its C++ destructor still runs at scope
   exit on the moved-from state. Reading `x` after move is an error, not a null read.
   RULING: this is the one place the "readable-as-null" property of the move ruling does
   not hold, because there is no null to read.
3. **`_ = move x;`** as canonical release runs the destructor immediately and marks `x`
   consumed. Consistent with the assignment transparency direction.
4. **By-value parameters.** A C++ `void f(T)` called with a CFlat lvalue copies (copy
   ctor) unless the argument is `move x`. A CFlat function `void g(move T x)` where `T` is
   foreign is exactly a C++ by-value parameter with caller-side move. Borrow-into-sink
   slot rule applies unchanged: you cannot `move` a borrowed `T*` into a by-value `T`.
5. **`alias T` returns** are the spelling of `T&` returns and carry the existing
   no-lifetime-extension meaning. The base plan's M2 note about reference returns is
   satisfied by reusing this keyword rather than inventing a reference type.
6. **const.** CFlat drops const (ruling 2026-08-26). Consequence for C++ overloads on
   constness: an lvalue argument prefers the non-const overload; the const overload is
   picked only when it is the sole candidate or the argument is a temporary. `const
   T&` parameters accept everything. RULING: accept this consequence, or lift the const
   ruling for foreign types only (not recommended: two const models in one language).
7. **Copy elision.** Guaranteed elision cases follow C++; CFlat must not insert a copy
   the C++ caller would not. Instrumented counters in the base plan M4 prove this.
8. **Interfaces.** A foreign class never implements a CFlat interface and a CFlat struct
   never derives from a foreign class before M10. `is`/`as` on a foreign pointer maps
   to `dynamic_cast` in M6; before that it is an error.

## Errors, diagnostics, and modes

- Every unsupported construct is a `LogError` at the CFlat use site, carrying Clang's
  diagnostic text after a `-` separator. Header parse errors that do not touch a
  requested declaration are suppressed (base plan M1).
- Potentially-throwing calls are an error until M8 lands: "call to 'ns.f' may throw;
  C++ exceptions are not supported yet". Detection is Clang's `noexcept` analysis, not
  a runtime guess.
- `--check`, `--out-lli`, `-o` all work from M2. `--run` is rejected for a program that
  needs C++ static initializers or EH until M9 verifies JIT loading of the C++ runtime.
- `expect_error` fixtures cover: name collision, private member, deleted ctor, move-only
  plain assignment, throwing call, reference field, unsupported template argument.

## Exceptions - surface options for M8 (decision deferred)

CFlat has no `try`/`catch`. Three options, cheapest first:

1. **Contain at the boundary.** Foreign exceptions never enter CFlat frames; the
   compiler wraps potentially-throwing calls and terminates with a diagnostic. Cheapest,
   no syntax, matches Swift's initial C++ interop stance.
2. **Propagate through, catch only in C++.** CFlat frames get correct cleanup but no
   catch syntax. This is the base plan M8 exit criterion.
3. **Add `try`/`catch` for foreign types.** Full surface. Not proposed for v1.

Recommendation: ship 1 with M2, upgrade to 2 at M8, keep 3 out of scope until a library
demands it. RULING at M8 start.

## First slice and ABI choice

The base plan's M1-M4 is too wide for a first release. Proposed thin slice:

- One ABI. Windows/MSVC first: it is the primary development host, the VS install
  supplies C++ standard headers, and `test.bat` is the maintainer's bar. macOS is
  second; its self-contained no-Xcode build has no C++ standard headers, so a stdlib
  profile for macOS is a prerequisite that must be solved before `test.sh` can cover
  any `std.` use. RULING: MSVC first, or Itanium first because two hosts run it.
- Scope: `noexcept` free functions and static members, scalars, pointers, references
  as above, trivially-copyable records by value, namespaces, scoped enums, overloads.
  No ctors/dtors, no templates, no virtual. This is base plan M2 + M3.
- Exit: a separately compiled C++ library with overloaded namespaced functions and a
  packed/over-aligned trivial struct is called from `Test/test_c_interop.cb` with the
  surface above, on one host, from cold and warm cache.

M4 (ctor/dtor/nontrivial) is the second slice and is where the reconciliation rules
above get their first real test.

## Rulings

Ruled 2026-09-06:

1. Import spelling: `import cpp "file"`, keyword before the path, extension irrelevant.
2. Prototype scope: primitives and bare pointers first; then trivial records by value
   (M3), then classes (M4), then polymorphism (M6).
3. Move-from: after `move x` on a stack-owned C++ object, `x` is compile-time consumed
   (use-after-move diagnostics apply) and its C++ destructor still runs at scope exit on
   the moved-from state. No destructor at the move point.
4. const overloads: an lvalue prefers the non-const overload; const is chosen only when it
   is the sole candidate or the argument is a temporary. Revisit when the prototype matures.
5. Construction spelling: `T(args)` call form for stack objects, `= default` for the default
   constructor.

Still open:

1. Per-import `std` clause, or CLI-only.
2. Overload sets: confirm foreign and native functions never mix.
3. Exceptions: option 1 (contain at the boundary) for the first slice.
4. First ABI: Itanium on macOS is what the prototype runs on today; MSVC unverified.

## Relationship to the base plan

| This document | Base plan milestone |
|---------------|---------------------|
| Import spelling, name lookup | M1 |
| Types table (scalars, pointers, refs), calls | M2 |
| Trivial records | M3 |
| Construction, lifetime reconciliation | M4 |
| Templates, operators | M5 |
| `is`/`as`, virtual | M6 |
| Callbacks (existing function pointer story) | M7 |
| Exceptions surface | M8 |
| `std.string` conversions, `--run` gating | M9 |

Repository constraints (both-pass `ParseDeclarationSpecifiers`, LogError only, ASCII, no
new test files without instruction, cache round-trip for new fields, grammar rules with
no predicates) apply as in the base plan.
