#pragma once
#include <tuple>
// T42: a C++ `const X&` / `X&&` parameter bound to the temporary a non-explicit converting
// constructor makes from an argument of another type (char*, a class, a template class).
namespace cpi_t42 {
inline int made = 0, gone = 0;
struct Src { int v; Src(int x) : v(x) {} };
struct Box {
    int v;
    Box(const char* s) : v(s ? (int)s[0] : -1) { made++; }
    Box(const Src& s) : v(s.v + 1000) { made++; }
    Box(const Box& o) : v(o.v) { made++; }
    Box(Box&& o) : v(o.v) { made++; }
    ~Box() { gone++; }
    int id() const { return v; }
};
struct Strict { int v; explicit Strict(const char*) : v(7) {} };
// T75: a cast direct-initializes, so explicit constructors are candidates (clang ranks them).
// Non-trivial destructors: a trivially-copyable target is a separate, still-open refusal.
struct ExplicitOnly { int v; explicit ExplicitOnly(const char* s) : v(s ? (int)s[0] : -1) {} ~ExplicitOnly() {} };
struct RankExplicit { int tag; explicit RankExplicit(char*) : tag(3) {} RankExplicit(const char*) : tag(2) {} ~RankExplicit() {} };
inline int fc(const Box& b) { return b.v + 100000; }
inline int fr(Box&& b) { return b.v + 200000; }
inline int fv(Box b) { return b.v + 300000; }
inline int two(const Box& b) { return b.v + 400000; }
inline int two(Box&& b) { return b.v + 500000; }
inline int exr(Strict&& s) { return s.v; }
inline Src mkSrc(int x) { return Src(x); }
struct Holder {
    int last = 0;
    int mc(const Box& b) { last = b.v; return b.v + 600000; }
    int mr(Box&& b) { last = b.v; return b.v + 700000; }
    int operator[](const Box& b) { return b.v + 800000; }
    Holder& operator+=(const Box& b) { last += b.v; return *this; }
    Holder& operator-=(Box&& b) { last = b.v; return *this; }
    int operator()(Box&& b) { return b.v + 50; }
};
struct Wrap { int v; Wrap(Box&& b) : v(b.v + 900000) {} };

template<class T> struct weak;
template<class T> struct shared { int owner = 0; };
template<class T> struct weak {
    int owner = 0;
    weak() = default;
    template<class U> weak(const shared<U>& s) : owner(s.owner) {}
    int id() const { return owner; }
};
inline int wc(const weak<int>& w) { return w.owner + 10; }
inline int wr(weak<int>&& w) { return w.owner + 20; }
// Map-like and vector-like overload pairs: clang binds a converted temporary to `K&&`.
template<class K> struct Table {
    int ids[4] = {0, 0, 0, 0};
    int vals[4] = {0, 0, 0, 0};
    int n = 0;
    int lastRef = 0;
    int& at(int id) {
        for (int i = 0; i < n; ++i) if (ids[i] == id) return vals[i];
        ids[n] = id; return vals[n++];
    }
    int& operator[](const K& k) { lastRef = 1; return at(k.id()); }
    int& operator[](K&& k) { lastRef = 2; return at(k.id()); }
    void push_back(const K& k) { lastRef = 1; at(k.id()) = 1; }
    void push_back(K&& k) { lastRef = 2; at(k.id()) = 2; }
    int insert(K&& k) { lastRef = 2; at(k.id()) = 3; return n; }
};
}

// Round 2: clang decides the `X&&` binding. Only an `operator X()` (accepted, operator picked);
// a ctor AND an operator (ambiguous); a genuine const source into a `T(S&)` ctor (cv loss).
namespace cpi_t42 {
struct OpTarget { int value; OpTarget(int v) : value(v) {} };
struct OpSource { int v = 70; operator OpTarget() const { return OpTarget(v + 7); } };
inline int takeOp(OpTarget&& t) { return t.value; }
inline int takeOpC(const OpTarget& t) { return t.value + 1000; }
struct AmbSource;
struct AmbTarget { int value; AmbTarget(const AmbSource&); };
struct AmbSource { operator AmbTarget() const; };
inline AmbTarget::AmbTarget(const AmbSource&) : value(41) {}
inline AmbSource::operator AmbTarget() const { return AmbTarget(*this); }
inline int takeAmb(AmbTarget&& t) { return t.value; }
struct CvSource { int value = 7; };
inline const CvSource& cvSource() { static const CvSource s; return s; }
inline CvSource& cvMutable() { static CvSource s; return s; }
struct CvTarget { int value; CvTarget(CvSource& s) : value(s.value + 1) {} };
inline int takeCv(CvTarget&& t) { return t.value; }
}

namespace cpi_t42 {
inline int t334_arrcast_gone = 0;
inline void t334_arrcast_reset() { t334_arrcast_gone = 0; }
inline int t334_arrcast_dtor_count() { return t334_arrcast_gone; }
struct T334ArrChar {
    int tag;
    T334ArrChar(char*) : tag(1) {}
    ~T334ArrChar() { t334_arrcast_gone++; }
    int id() const { return tag; }
};
struct T334ArrIntPtr {
    int tag;
    T334ArrIntPtr(int*) : tag(2) {}
    ~T334ArrIntPtr() { t334_arrcast_gone++; }
    int id() const { return tag; }
};
struct T334ArrConstChar {
    int tag;
    T334ArrConstChar(const char*) : tag(3) {}
    ~T334ArrConstChar() { t334_arrcast_gone++; }
    int id() const { return tag; }
};
struct T334ArrPtrOrInt {
    int tag;
    T334ArrPtrOrInt(char*) : tag(21) {}
    T334ArrPtrOrInt(int) : tag(22) {}
    ~T334ArrPtrOrInt() { t334_arrcast_gone++; }
    int id() const { return tag; }
};
struct T334ArrRef {
    int tag;
    T334ArrRef(char (&)[4]) : tag(32) {}
    ~T334ArrRef() { t334_arrcast_gone++; }
    int id() const { return tag; }
};
struct T334ArrAmbiguous {
    int tag;
    T334ArrAmbiguous(char*) : tag(31) {}
    T334ArrAmbiguous(char (&)[4]) : tag(32) {}
    ~T334ArrAmbiguous() { t334_arrcast_gone++; }
    int id() const { return tag; }
};
struct T334ArrImplicit {
    int tag;
    T334ArrImplicit(char*) : tag(41) {}
    ~T334ArrImplicit() { t334_arrcast_gone++; }
    int id() const { return tag; }
};
struct T334ArrTupleRef {
    int tag;
    T334ArrTupleRef(std::tuple<const char (&)[4]> source) : tag(std::get<0>(source)[0]) {}
    ~T334ArrTupleRef() {}
    int id() const { return tag; }
};
struct T334ArrSource { char buf[4]; };
inline int t334_arrcast_first(char* p) { return p[0]; }
}

namespace cpi_t42 {
inline Box t334_make_box(int value) { return Box(Src(value)); }
}

namespace cpi_t42 {
inline int t339_born = 0, t339_copies = 0, t339_moves = 0, t339_dead = 0;
inline void t339_reset() {
    t339_born = t339_copies = t339_moves = t339_dead = 0;
}
struct T339Box {
    int value;
    explicit T339Box(int v) : value(v) { ++t339_born; }
    T339Box(const T339Box& other) : value(other.value) { ++t339_born; ++t339_copies; }
    T339Box(T339Box&& other) noexcept : value(other.value) {
        ++t339_born; ++t339_moves; other.value = -1;
    }
    ~T339Box() { ++t339_dead; }
    int id() const { return value; }
};
inline T339Box t339_make_box() { return T339Box(7); }
inline T339Box t339_outer_box(T339Box value) { return T339Box(value.id() + 2); }
}
