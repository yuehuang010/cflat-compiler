# C++ nested member class as a free-function return; assignment to an operator()/operator* class result

## Summary
Two leftovers found next to t30 (MSVC bitset proxy), both pre-existing on master and not caused by
the lazy nested member-class completion that t30 fixed:

1. A FREE function returning a member class of a class template specialization by value is not
   bound: `'r2.freeMake' was not bound: return type 'r2::Box<3>::Item' is unsupported`. clang: 18.
   Member functions returning the same type bind (they are retried on use; free functions are not).
2. Assigning to a class prvalue returned by `operator()` or unary `operator*` is refused with
   "Left side of assignment is not an addressable lvalue.", even when the proxy is a top-level
   template and there is no const sibling. `operator[]` and named methods returning the same proxy
   accept `f(i) = v` (they route to the proxy's `operator=`).

## Repro
```cpp
// lazy.hpp
namespace r2 {
template<int N> struct Box { struct Item { int v; int twice() const { return v * 2; } }; };
inline Box<3>::Item freeMake(int x) { return Box<3>::Item{x}; }
}
// ctl2.hpp
namespace r2d {
template<unsigned long N> class Bits;
template<unsigned long N> class Ref {
public:
    Ref(Bits<N>* b, unsigned long p) : b(b), pos(p) {}
    Ref& operator=(bool v);
    Bits<N>* b; unsigned long pos;
};
template<unsigned long N> class Bits {
public:
    Ref<N> operator()(unsigned long p) noexcept { return Ref<N>(this, p); }
    Ref<N> operator*() noexcept { return Ref<N>(this, 0); }
    unsigned long word = 0;
};
template<unsigned long N> Ref<N>& Ref<N>::operator=(bool v)
{ if (v) b->word |= (1ul << pos); else b->word &= ~(1ul << pos); return *this; }
}
```
```cflat
import cpp "lazy.hpp";
extern int main() { if (r2.freeMake(9).twice() != 18) return 1; return 0; }   // not bound

import cpp "ctl2.hpp";
extern int main() { r2d.Bits<8> b = default; b(2) = true; *b = true; return b.word == 5 ? 0 : 1; }
// "Left side of assignment is not an addressable lvalue." at b(2) = true and at *b = true
```
Both reproduce cold and warm on macOS and Windows, master and the t30 fix alike.

## Fix direction
1. Free C++ functions: retry a by-value nested member-class return on use (request the enclosing
   specialization with definitions, as TryBindRefusedCxxMember does for members).
2. The assignment lvalue check should treat a C++ class prvalue from operator()/operator* like the
   operator[] / named-method result: dispatch to the class's operator=.
