#pragma once
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
