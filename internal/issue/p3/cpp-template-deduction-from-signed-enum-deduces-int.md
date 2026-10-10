# C++ template deduction from a signed-backed unscoped enum deduces int

Found by the T49 Sol review (2026-10-05) as PRE-EXISTING: master and the T49 branch agree.

## Repro

```cpp
// review.hpp
namespace rv {
enum U : int { v = -3 };
template<class T> int deduce(T) { return std::is_enum_v<T> ? 7 : 9; }
}
```

```cflat
import cpp "review.hpp";
extern int main() { return rv.deduce(rv.U.v); }
```

CFlat returns 9 (T deduced as int); clang++ -std=c++20 returns 7 (T = rv::U).

## Root cause (suspected)

The template-argument spelling for the call is taken from the argument's lowered integer type,
not its source enum identity. T49 taught CxxDistinctEnumArgument to recover the identity via
InferSourceTypeName for distinct-enum refusal; deduction does not use that path.

## Fix direction

Deduce from the source enum identity (InferSourceTypeName / NamedVariable), never from the
lowered int; cover signed, unsigned and implicit-backed unscoped enums plus CFlat enums.
