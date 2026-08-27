# `where T : IFace<T>` does not parse, though the manual documents it

Filed 2026-08-26, found while reviewing `where` for the generic value-parameter design
(`internal/plan/generic-value-parameters.md`).

`doc/LANGUAGE.md:774` documents this exact spelling as THE example of a generic type
constraint:

```cflat
T maxOf<T>(T a, T b) where T : IComparable<T>
{
    return a.CompareTo(b) >= 0 ? a : b;
}
```

It does not compile. Measured on `x64/Release/cflat`:

## Repro

Function scope:

```cflat
interface ICmp<T> { int cmp(T o); };

T maxOf<T>(T a, T b) where T : ICmp<T> { return a; }
extern int main() { return 0; }
```
```
repro.cb(3,35): error: found '<' but expected {'lock', '{'}
    T maxOf<T>(T a, T b) where T : ICmp<T>
                                       ^
```

Struct scope fails identically, so this is not scope-specific:

```cflat
struct Holder<T> where T : ICmp<T> { T v = default; };
```
```
repro.cb(3,31): error: found '<' but expected '{'
```

The non-generic form parses and works: `where T : IShow` is fine.

## Root cause

```antlr
typeParameterConstraint
    : Identifier ':' Identifier          // CFlat.g4:418
```

The constraint target is a bare `Identifier`. A generic interface is
`Identifier genericTypeParameters` (`ICmp` + `<T>`), so the `<` has nowhere to go and the
parser reports the next expected token. The same rule also rejects any primitive on the
right (`where T : int` -> "found 'int' but expected Identifier"), because `int` is a keyword
token rather than an `Identifier` - not needed today, but it shows the rule is a
name-to-name binding rather than a name-to-TYPE binding.

## Impact

The canonical constraint - "T must implement the comparison interface for T" - cannot be
written at all. Constraints are optional, so nothing is blocked outright, but a reader
copying the documented example gets a parse error, and generic code that should be
constrained ships unconstrained instead.

## Workaround

Omit the constraint. Verified - the same program compiles and the generic still resolves the
interface method through the concrete instantiation:

```cflat
interface ICmp<T> { int cmp(T o); };
class Num : ICmp<Num> { int v = default; int cmp(Num o) { return v - o.v; } };

T maxOf<T>(T a, T b) { return a.cmp(b) >= 0 ? a : b; }   // no where clause: compiles
```

What is lost is the instantiation-site check: a `T` that does not implement the interface
now fails later, inside the body, with a method-resolution error instead of a constraint
diagnostic naming the interface. Constraining by a NON-generic interface
(`where T : IShow`) also works if one exists.

## Fix direction

Widen the right-hand side of `typeParameterConstraint` from `Identifier` to a type - at
minimum `genericIdentifier`, which covers both `IShow` and `ICmp<T>` - and validate the
target in the listener (it must name an interface, and its type arguments must resolve in
the enclosing generic's substitution).

**Land this together with Stage 4 of `internal/plan/generic-value-parameters.md`**, which
widens the same rule again to admit value predicates (`where N > 0`). Doing them separately
means touching one small grammar rule twice and regenerating ANTLR twice.

Regression: extend an existing generics test rather than adding a file; a negative case for
a non-implementing `T` belongs in `Test/errors/` via `expect_error`.
