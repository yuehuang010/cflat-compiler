# Implicit converting constructors (native CFlat + C++ bridge)

Status: RULED 2026-09-26. Phases 1 + 3 landed (shared resolver
`LLVMBackend::ConvertThroughCxxConvertingCtor`, C++ class targets at init, assignment, field /
element store, return, brace element); phase 2 (native CFlat positions) and phase 4 (docs) open.
Replaces the p4 issue `cpp-converting-constructor-at-copy-init-and-assignment.md` (deleted; its
repro is below).

Phase 3 leftovers, for phase 2 to pick up: a TRIVIALLY-COPYABLE C++ class (`std.optional<int>`,
a POD with a one-arg ctor) takes the native value path, not the C++ construct-into-slot path, so
it converts only once phase 2 wires that path; a `?:` of scalars into a C++ class local or
return (`T t = c ? 1 : 2;`, `return c ? 1 : 2;` - refused cleanly) and a global initializer are
still refused. A pointer source converts at init, return and a call argument (`P p = &k;`, `take(&k)`
into `P(const int*)`) for a proven single-level primitive pointer; a pointer to a class is not
offered, and the address-of operand of a binary operator is issue
`p3/cpp-operator-address-of-operand-not-converted.md`. Assignment follows C++ ranking: an exact
`operator=(U)`, then a template
`operator=<U>`, then any other direct `operator=` beats the converting constructor, each tried as
the real call with diagnostics suppressed (a probe and the call cannot disagree). When no direct
one is callable but C++ would call an arithmetic `operator=(U)` that CFlat's call rules refuse
(`int` into `operator=(double)`), the assignment is refused naming it, never silently rerouted. Direct `operator=(U)` is used only for a provably live destination; conditionally-live locals and all globals take `T(u)` + move-assign (operator= on a possibly unconstructed object is UB). A C++ target converts through
the call-argument classifier, so it accepts what a C++ call argument accepts today - including a
narrowing `double -> int` constructor argument, which point 4 refuses; ruling needed.

## Ruling (maintainer, 2026-09-26)

- A constructor callable with ONE argument is a converting constructor: where a value of type
  `T` is expected and the source is some other type `U`, the compiler inserts `T(source)`.
- Applies to BOTH native CFlat structs and imported C++ classes.
- Native CFlat: EVERY one-argument constructor converts. There is NO `explicit` opt-out for now
  - do not add an `explicit` keyword, attribute or soft keyword.
- C++ classes: C++'s own rule - non-`explicit` converts, `explicit` stays refused (7897acf2
  message at call arguments; same message family at the new positions).
- String literals (earlier same-day ruling, now a special case of this one): `"..."` into a class
  whose converting constructor takes `const char*` selects that constructor at COMPILE time and
  passes the literal's constant pointer. Never build a runtime CFlat `string` and convert it.

## Semantics: "as if the user wrote `T(source)`"

Every implicit conversion must be exactly equivalent to the explicit spelling at that position -
same overload chosen, same temporary, same ownership, same destruction point. That one sentence is
the spec; anything that behaves differently from the explicit form is a bug.

1. **Positions** (all copy-initialization contexts, per "return follows assignment rules" and
   "`=` is total over T"): variable init, assignment, field store, array element store, call
   argument, return, designated / positional brace-init element (a brace list itself is never the
   SOURCE - `S s = {3.0};` keeps today's aggregate meaning).
2. **Assignment** `t = u;`: if `T` has an assignment operator taking `U` (C++
   `std::string::operator=(const char*)`), that overload wins, as in C++ overload resolution.
   Otherwise build `T(u)` and assign it as a moved temporary (the normal owning-temporary path).
3. **One user-defined conversion.** No chaining: `char*` -> `std.string` -> `Wrapper` is refused;
   a converting constructor never combines with an `operator T` conversion.
4. **The source must pass the constructor's parameter under CFlat CALL rules** - no implicit
   narrowing (2026-09-04), pointer is not a number (2026-09-26), int -> bool the one exception.
   So `Meters m = 3;` works iff `Meters(3)` works.
5. **Overload ranking** at calls: an exact / standard-conversion match of another overload beats a
   user-defined conversion. Two converting paths of equal rank -> ambiguity error listing both.
6. **Same-type sources are not conversions.** `T -> T` stays the copy/move path; a one-argument
   `T(T)`-shaped constructor is not a converting constructor.
7. **Generics:** a generic struct's constructor converts once instantiated; no deduction of the
   struct's type arguments from the source (`Box<T> b = 3;` needs `Box<int>` spelled).
8. **Compile time only.** Constructor selection is static; nothing is dispatched at run time.

## Today (master cfdbeca4, probed 2026-09-26)

| Position | Native CFlat | C++ class |
|---|---|---|
| call argument | refused (`no overload of 'walk' matches`) | WORKS (7897acf2; `explicit` refused) |
| init `T t = u;` | refused (`cannot store a single scalar value into struct storage`) | refused (`cannot initialize C++ class ...`) |
| assignment `t = u;` | refused (same scalar message) | refused (`cannot assign to C++ class ...`) |
| return, field, element | refused | refused |

The C++ call-argument path is the model: find the 7897acf2 resolution code and reuse it for the
other positions and for native structs, rather than writing a second mechanism.

## Repros (scratch/conv/ has n1-n3 and c1)

```cflat
// native
struct Meters { double v = 0; Meters(double x) { v = x; } };
double walk(Meters m) { return m.v; }
Meters twice(double x) { return x * 2.0; }        // return
extern int main()
{
    Meters b = 3.0;                               // init
    b = 4.0;                                      // assignment
    double w = walk(2.0);                         // call argument
    Meters r = twice(1.0);
    return (b.v == 4.0 && w == 2.0 && r.v == 2.0) ? 0 : 1;
}
```

```cpp
// cv.h - C++ side (template + plain converting ctors, operator= by value)
#pragma once
namespace cv {
struct Str { const char* p = nullptr; int n = 0; Str() = default;
    Str(const char* s) : p(s) { while (s[n]) ++n; }
    Str(const Str& o) : p(o.p), n(o.n) {} Str& operator=(const Str& o) { p = o.p; n = o.n; return *this; } ~Str() {} };
struct Val { long v = 0; Val() = default; template <class T> Val(T x) : v((long)x) {}
    Val(const Val& o) : v(o.v) {} Val& operator=(Val o) { v = o.v; return *this; } ~Val() {} };
}
```

```cflat
import cpp "cv.h";
extern int main()
{
    cv.Str s = "world";   cv.Val v = 3;           // init (plain, template ctor)
    s = "again";          v = 4;                  // assignment
    return (s.n == 5 && v.v == 4) ? 0 : 1;
}
```

## Phases

1. **Shared resolver.** One helper: given target type `T`, source value/type `U` and a position,
   return the chosen constructor (native or C++ incl. template instantiation through clang) or a
   refusal reason (explicit, ambiguous, no viable, chained). Extract from the 7897acf2 call path.
2. **Native positions.** Wire init, assignment, field/element store, brace element, return, and
   native call-argument overload resolution to the resolver. Native call-argument ranking (point 5)
   is new work in `functionTable` overload resolution.
3. **C++ positions.** Same wiring for C++ classes; assignment checks `operator=(U)` first. Replace
   the two "cannot initialize / cannot assign to C++ class" refusals with the resolver's reason when
   it fails.
4. **Docs.** `doc/LANGUAGE.md` Structs section (native rule, no `explicit`), the conversion table
   near `operator T`, and the C++ interop section.

## Constraints for the implementing agent

- Type-parsing changes (none expected) go to BOTH `ParseDeclarationSpecifiers()` copies.
- If a new field lands on `StructData` / `TypeAndValue` (e.g. a cached "has one-arg ctor" flag),
  add it to the `LLVMBackend.cpp` `--init` cache round-trip in the same change.
- `LogError` / `LogErrorContext` only; ASCII; never edit `cflat/locales/`.
- Watch the ownership path: the implicit temporary must be the same owned rvalue `T(u)` produces
  (destroyed after the call at an argument; moved into the slot at init/assignment). Add a leg
  with a destructor-counting struct so a double destroy or leak shows.
- Existing diagnostics that tests expect (`cannot store a single scalar value into struct
  storage` for a struct WITHOUT a one-arg ctor, the 7897acf2 explicit message) must keep firing -
  grep `Test/errors/` before changing a message.

## Acceptance

- Native legs (extend an existing native struct/constructor test in `Test/`): all positions from
  the repro, a destructor-count leg, a generic struct leg, an overload-ranking leg (exact overload
  beats conversion).
- C++ legs: `cv.h` into `Test/library/cpp_interop_explicit.h` (or the closest existing fixture),
  legs in `Test/test_cpp_interop_bridge.cb`: init, assignment via `operator=(const char*)`-style
  direct overload AND via temporary, return, field store, string literal into `const char*` ctor.
- New `Test/errors/err_*.cb`: C++ `explicit` at init and assignment; ambiguity (two converting
  ctors of equal rank); chained conversion; narrowing through a converting ctor
  (`struct I { I(int x) {} }; I i = 3.5;`); pointer into an integer-taking ctor.
- `./test.sh Release` green; C++ header-parse budget (cold 1 / warm 0) unchanged.
- Local only, never in the suite: `bash scratch/probe3/run.sh` - f05 (fmt `std.string = "..."`)
  and j03 (nlohmann `j["x"] = 3`) pass. Eigen `c = a * 3.0` also needs
  `internal/issue/p3/cpp-operator-returning-unrequested-specialization-not-bound.md`.
