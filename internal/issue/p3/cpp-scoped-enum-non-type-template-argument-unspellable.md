# A scoped-enum NON-TYPE template argument cannot be spelled, and the diagnostic blames the class

Found 2026-09-16 by the C++-interop bug bash round 3 (macOS arm64, Release, worktree at master 2798eb1a).

## Summary

For `template <Color C> struct ColorBox` where `Color` is an `enum class`, no CFlat spelling
instantiates the template. `bb3e.ColorBox<bb3e.Color.Green>` lowers the argument to the bare
integer `2`, so the request that reaches clang is `bb3e::ColorBox<2>` - which is ill-formed for a
scoped-enum parameter (no implicit int -> `enum class` conversion) - and the compiler reports

```
'bb3e::ColorBox<2>' does not name a C++ class type in the imported headers
```

which points at the class, not at the argument that was mis-spelled. `::`-qualified spelling is a
parse error, and the bare integer fails the same way.

An UNSCOPED enum non-type argument (`bb3e.PBox<bb3e.P_TWO>`), a plain `int` non-type argument
(`bb3e.NBox<3>`) and a scoped enum used as a TYPE argument (`bb3e.TBox<bb3e.Color>`) all work, so
this is specific to a scoped enum in non-type position.

## Repro

`scratch/bb3_enum.h`:

```cpp
namespace bb3e {
enum class Color : unsigned char { Red = 1, Green = 2, Blue = 200 };
enum Plain { P_ONE = 1, P_TWO = 2 };
template <Color C> struct ColorBox { int get() const { return (int)C; } };
template <Plain P> struct PBox    { int get() const { return (int)P; } };
template <int N>   struct NBox    { int get() const { return N; } };
template <typename T> struct TBox { T v; TBox(T x) : v(x) {} int as_int() const { return (int)v; } };
}
```

`scratch/bb3_ecolor.cb`:

```cflat
import cpp "bb3_enum.h";
extern int main()
{
    bb3e.ColorBox<bb3e.Color.Green> cb = default;
    return cb.get();
}
```

Measured (compile exit 1, three spellings, two fresh runs):

```
bb3e.Color.Green   -> bb3_ecolor.cb(5,17): 'bb3e::ColorBox<2>' does not name a C++ class type in the imported headers
bb3e.Color::Green  -> parse error: cannot understand the code at '... bb3e.Color::'
2                  -> bb3_ecolor.cb(5,17): 'bb3e::ColorBox<2>' does not name a C++ class type in the imported headers
```

Working controls (compile 0, run 0): `bb3e.NBox<3>.get() == 3`, `bb3e.PBox<bb3e.P_TWO>.get() == 2`,
`bb3e.TBox<bb3e.Color>(bb3e.Color.Blue).as_int() == 200`.

## Root cause

The C++ type-request spelling built for a template argument renders an enumerator as its integer
VALUE. That is right for `int`/unscoped-enum parameters (the enumerator converts) and wrong for a
scoped-enum parameter, which requires the qualified enumerator (`bb3e::Color::Green`) or an
explicit cast (`static_cast<bb3e::Color>(2)`, or `(bb3e::Color)2`).

## Fix direction

When the template parameter's declared type is a scoped enumeration - or, cheaper and sufficient,
whenever the argument expression's CFlat type is a bound C++ scoped enum - render the argument as
`(<qualified enum type>)<value>` in the type request. Independently, the "does not name a C++ class
type" message should name the argument that was rendered, since the requested spelling is what the
user cannot see.

Suggested bucket: p3 (capability gap, clean-ish failure, no wrong values; the misleading message is
the sharpest part).
