#pragma once

namespace mix15r {
// A C++ member/free operator or method that returns T& or const T&: a parenthesized result
// names the referent for member access (`(r << 2).v`), as a call result does.
struct Ref {
    int v;
    explicit Ref(int x = 0) : v(x) {}
    Ref& operator+(int x) { v += x; return *this; }
    int get() const { return v * 10; }
    Ref& bump() { ++v; return *this; }
};
inline Ref& operator<<(Ref& r, int x) { r.v <<= x; return r; }
inline const Ref& operator>>(const Ref& r, int x) { return r; }
}
