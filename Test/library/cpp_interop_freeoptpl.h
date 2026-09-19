#pragma once
// Fixture for the free-operator-TEMPLATE binding path (return codes 2100-2149). libc++ declares
// every basic_string operator as a free function template over the class template; these classes
// reproduce that shape in a header the fixture controls, so the coverage is not std::string
// specific. Nt/NtFree hold the ACCEPT SET: non-template free operators, which must keep binding
// through the path that already handled them.
#include <compare>

namespace cppfop {

// A class TEMPLATE whose binary operators are FREE FUNCTION TEMPLATES. Each operator adds a
// distinct tag to its result so a leg can tell which overload C++ selected.
template <class T>
class Box {
public:
    Box() : v_(0) {}
    explicit Box(T v) : v_(v) {}
    T v() const { return v_; }
    T v_;
};

template <class T> Box<T> operator+(const Box<T>& a, const Box<T>& b)
{ return Box<T>(a.v_ + b.v_ + 1); }
template <class T> Box<T> operator+(const Box<T>& a, T b) { return Box<T>(a.v_ + b + 2); }
template <class T> Box<T> operator+(T a, const Box<T>& b) { return Box<T>(a + b.v_ + 3); }
template <class T> Box<T> operator-(const Box<T>& a, const Box<T>& b)
{ return Box<T>(a.v_ - b.v_ + 4); }
template <class T> Box<T>& operator|(Box<T>& a, const Box<T>& b)
{ a.v_ += b.v_; return a; }
template <class T> Box<T> operator^(Box<T>& a, const Box<T>& b)
{ a.v_ += b.v_; return a; }
template <class T> bool operator==(const Box<T>& a, const Box<T>& b) { return a.v_ == b.v_; }
template <class T> bool operator!=(const Box<T>& a, const Box<T>& b) { return a.v_ != b.v_; }
template <class T> bool operator<(const Box<T>& a, const Box<T>& b) { return a.v_ < b.v_; }
template <class T> bool operator>(const Box<T>& a, const Box<T>& b) { return a.v_ > b.v_; }
template <class T> bool operator<=(const Box<T>& a, const Box<T>& b) { return a.v_ <= b.v_; }
template <class T> bool operator>=(const Box<T>& a, const Box<T>& b) { return a.v_ >= b.v_; }

// A second class template that declares only == and <=> as free templates, the C++20 shape
// libc++ uses for basic_string: the four relational operators exist only as REWRITES.
template <class T>
class Ord {
public:
    Ord() : v_(0) {}
    explicit Ord(T v) : v_(v) {}
    T v() const { return v_; }
    T v_;
};

template <class T> bool operator==(const Ord<T>& a, const Ord<T>& b) { return a.v_ == b.v_; }
template <class T> std::strong_ordering operator<=>(const Ord<T>& a, const Ord<T>& b)
{ return a.v_ <=> b.v_; }

// ACCEPT SET: a plain class with NON-template free operators. These bound before the template
// path existed and must keep binding through the same candidate scan.
class Nt {
public:
    Nt() : n_(0) {}
    explicit Nt(int n) : n_(n) {}
    int n() const { return n_; }
    int n_;
};

inline Nt operator+(const Nt& a, const Nt& b) { return Nt(a.n_ + b.n_ + 10); }
inline bool operator==(const Nt& a, const Nt& b) { return a.n_ == b.n_; }
inline bool operator<(const Nt& a, const Nt& b) { return a.n_ < b.n_; }

}
