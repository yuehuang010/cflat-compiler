#pragma once

namespace cppvarctor {
struct Cnt {
    inline static int copies = 0;
    inline static int moves = 0;
    inline static int ctors = 0;
    inline static int dtors = 0;
    int value;
    Cnt(int v) : value(v) { ++ctors; }
    Cnt(const Cnt& other) : value(other.value) { ++copies; ++ctors; }
    Cnt(Cnt&& other) noexcept : value(other.value) { ++moves; ++ctors; other.value = -99; }
    ~Cnt() { ++dtors; }
};

inline void sink(Cnt& c) { ++c.value; }

struct VariadicL {
    template<class... A> VariadicL(A&... a) { (sink(a), ...); }
};

struct VariadicR {
    template<class... A> VariadicR(A&&... a) { (sink(a), ...); }
};

struct Base { Base(Cnt& c) { sink(c); } };
struct Derived : Base { using Base::Base; };

struct Mixed {
    int value;
    template<class U, class... A>
    Mixed(U& u, A... a) : value(((int)a + ... + 0)) { sink(u); }
};

inline void reset() { Cnt::copies = Cnt::moves = Cnt::ctors = Cnt::dtors = 0; }
inline int copies() { return Cnt::copies; }
inline int moves() { return Cnt::moves; }
inline int ctors() { return Cnt::ctors; }
inline int dtors() { return Cnt::dtors; }
}
