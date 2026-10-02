# Variadic C++ alias template from an in-repo header refused

Found by ST4 (2026-10-01), pre-existing. An alias template with a parameter pack, e.g.

```cpp
template<unsigned long... I> struct Seq { static constexpr unsigned long size() { return sizeof...(I); } };
template<unsigned long... I> using ISeq = Seq<I...>;
```

is refused when named from CFlat (`ns.ISeq<0,1,2>`): the alias machinery has no pack support
(LLVMBackend_Interfaces.cpp ~432). `std.index_sequence` works only because system headers are not
scanned at import (it goes through a clang type request). Fix: expand packs in the alias
substitution, or route an in-repo variadic alias through the same clang type request path.
