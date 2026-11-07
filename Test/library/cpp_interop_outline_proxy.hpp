#pragma once
// Proxy returned by value through operator(), unary operator*, operator[] and a named method, with
// operator= (bool, int) and operator|= defined OUT OF LINE on the class template (NESTPROXY).
namespace outp {
template<unsigned long N> class Bits;
template<unsigned long N> class Ref {
public:
    Ref(Bits<N>* b, unsigned long p) : b(b), pos(p) {}
    Ref& operator=(bool v);
    Ref& operator=(int v);
    Ref& operator|=(bool v);
    Bits<N>* b;
    unsigned long pos;
};
template<unsigned long N> class Bits {
public:
    Ref<N> operator()(unsigned long p) noexcept { access = 1; return Ref<N>(this, p); }
    Ref<N> operator*() noexcept { access = 2; return Ref<N>(this, 0); }
    Ref<N> operator[](unsigned long p) noexcept { access = 3; return Ref<N>(this, p); }
    Ref<N> at(unsigned long p) noexcept { access = 4; return Ref<N>(this, p); }
    unsigned long word = 0;
    int access = 0;
    int op = 0;
};
template<unsigned long N> Ref<N>& Ref<N>::operator=(bool v) {
    if (v) b->word |= (1ul << pos); else b->word &= ~(1ul << pos);
    b->op = 10; return *this;
}
template<unsigned long N> Ref<N>& Ref<N>::operator=(int v) {
    if (v) b->word |= (1ul << pos); else b->word &= ~(1ul << pos);
    b->op = 20; return *this;
}
template<unsigned long N> Ref<N>& Ref<N>::operator|=(bool v) {
    if (v) b->word |= (1ul << pos);
    b->op = 30; return *this;
}
// A proxy with no assignment operator: the prvalue assignment stays refused.
template<unsigned long N> class NoAssignRef {
public:
    NoAssignRef(Bits<N>* b) : b(b) {}
    Bits<N>* b;
};
template<unsigned long N> class NoAssignBits {
public:
    NoAssignRef<N> operator()(unsigned long) { return NoAssignRef<N>(nullptr); }
};
}
