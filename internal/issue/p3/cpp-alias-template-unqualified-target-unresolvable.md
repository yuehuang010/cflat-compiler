# A C++ alias template whose target clang prints UNQUALIFIED cannot be resolved at all

Bucket: p3 (compile failure on a legal header spelling, no silent wrong data).

## Summary

`template <class T> using X = Target<T>;` binds only when clang prints the target of the alias
as a NAMESPACE-QUALIFIED name. Two legal shapes print it unqualified and then neither the
CONSTRUCTOR spelling nor the DECLARATION spelling resolves:

- the target template lives in the alias's OWN namespace
  (`namespace n { template<class T> struct Cell{}; template<class T> using CellA = Cell<T>; }`);
- the target template is at GLOBAL scope (`template<class T> struct GlobalBox{};`
  `template<class T> using GlobalBoxAlias = GlobalBox<T>;`).

The alias-template CONSTRUCTOR spelling was fixed separately (commit that landed
`IsGenericBaseAlias` hops in `MainListener_PostfixExpression.cpp`); that fix is orthogonal - it
routes the call to the declaration path, and the declaration path is broken for these two shapes
too, so both spellings fail together.

## Repro

`scratch/b2_tpl4.h` + `scratch/b2_tpl5.cb` (target in the alias's own namespace):

```cpp
namespace b2s
{
    template <class T> struct Cell { T value; Cell() noexcept : value(T(5)) {} };
    template <class T> using CellA = Cell<T>;
}
```

```cflat
import cpp "b2_tpl4.h";
extern int main() { b2s.CellA<int> c = default; printf("v=%d\n", c.value); return 0; }
```

```
b2_tpl5.cb(2,34): cannot find the type 'Cell<int>'
```

Global-scope target - `scratch/b2_gbox2.cb` against `Test/library/cpp_interop_tpl.h`:

```
b2_gbox2.cb(2,34): cannot find the type 'GBoxAlias<int>'
```

The working shape, for contrast, is a qualified target: `namespace a { template<class T> using
CellAlias = other_ns::AliasCell<T>; }` resolves in both spellings (legs 1581-1586 in
`Test/test_cpp_interop_template.cb`).

## Root cause

`RawTypedef::cxxAliasPattern` stores the target as CLANG PRINTED IT. For a target in the alias's
own namespace or at global scope the printed pattern carries no namespace, so the harvested
target base ("Cell", "GlobalBox") is not a name the backend can look up from the use site: the
request is made for a base that does not exist under that spelling, and the error names the bare
target rather than the alias.

## Fix direction

Qualify the printed pattern at harvest time - print the target with a fully-qualified policy (or
re-qualify against the alias's declaration context) before storing it in `cxxAliasPattern`. This
is a CACHE PAYLOAD SEMANTICS change: `cxxAliasPattern` is serialized into the C header disk cache,
so the change needs a cache version bump alongside it, and any consumer that re-parses the pattern
has to keep accepting the already-qualified form.
