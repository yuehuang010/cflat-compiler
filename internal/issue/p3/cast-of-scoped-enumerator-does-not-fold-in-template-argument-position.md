Bucket: p3 (capability gap after a correct rejection; only workaround is a hardcoded literal)

# `(int)E.Member` does not fold as a non-type template argument

Found 2026-09-18 in review of fix/cpp-enum-nttp (macOS arm64, Release). Pre-existing.

## Summary

fix/cpp-enum-nttp made a scoped enumerator into an `int` non-type template parameter an honest
refusal (C++ rejects it too: `NBox<Color::Green>` is ill-formed, `NBox<(int)Color::Green>` is the
legal spelling). CFlat has no working equivalent of that legal spelling: the cast in template
argument position does not reach the NTTP integer folder, so the enumerator's NAME cannot be used
at all - only the literal value.

## Repro

Header (Test/library/cpp_interop_scoped_enum.h shape):
```cpp
namespace rev { enum class Color : unsigned char { Red = 1, Green = 2 };
                template <int N> struct NBox { int get() const { return N; } }; }
```

```cflat
import cpp "rev.h";
extern int main()
{
    rev.NBox<(int)rev.Color.Green> b = default;   // measured: exit 1,
    // "value argument '(int)rev.Color.Green' does not fold to an integer"
    return b.get();                                // expected 2
}
```

Controls: the same cast in an EXPRESSION folds (`return (int)rev.Color.Green;` -> 2); an unscoped
or pure-CFlat enumerator into an `int` NTTP works; `const int k = (int)rev.Color.Green;` at file
scope is refused for a different reason (internal/issue/p3/global-const-from-enumerator-not-constant.md).

## Root cause (hypothesis)

The NTTP value-argument path folds a `shiftExpression` through `FoldCompileTimeInt`
(ForwardRefScanner::ResolveForwardTypeArg and the MainListener twin), and that folder handles
integer literals, arithmetic and plain enumerators but not a cast expression over an enumerator.

## Fix direction

Teach the compile-time integer folder a cast of a foldable operand to an integer type (both
scanner and codegen copies), so `(int)E.M` and `(int)E.M + 1` fold in NTTP position; add a value
leg next to the enum-nttp legs in Test/test_cpp_interop_template.cb.

Also from the same review, minor: the "does not name a C++ class type" caret still points at the
class name column rather than the argument, although the text now names the rendered argument.
