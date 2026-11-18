#pragma once
// PRVARG: a by-value class prvalue beside a later call argument reaches a function template
// through a wrapper. Clang elides the prvalue into the template's parameter: no move, one dtor.
namespace prvarg {
inline int ctorCount_ = 0;
inline int copyCount_ = 0;
inline int moveCount_ = 0;
inline int dtorCount_ = 0;
inline int sideCount_ = 0;
inline void resetCounts() { ctorCount_ = copyCount_ = moveCount_ = dtorCount_ = sideCount_ = 0; }
inline int ctorCount() { return ctorCount_; }
inline int copyCount() { return copyCount_; }
inline int moveCount() { return moveCount_; }
inline int dtorCount() { return dtorCount_; }
inline int sideCount() { return sideCount_; }
inline int next_side() { return ++sideCount_; }
struct Ev {
    int value;
    Ev(int n) : value(n) { ++ctorCount_; }
    Ev(const Ev& other) : value(other.value) { ++copyCount_; }
    Ev(Ev&& other) : value(other.value) { ++moveCount_; }
    ~Ev() { ++dtorCount_; }
};
template<class T> int free_tmpl(T x, int y) { return x.value * 1000 + y; }
template<class T, class U> int pair_tmpl(T a, U b) { return a.value * 1000 + b.value; }
// Later arguments that convert at the call (narrowing, derived-to-base) stay in the wrapper.
template<class T> int narrow_long(T x, int y) { return y; }
template<class T> int narrow_double(T x, int y) { return y; }
template<class T> int narrow_short(T x, short y) { return y; }
template<class T> int narrow_char(T x, char y) { return y; }
template<class T> int narrow_float(T x, float y) { return (int)(y * 2); }
struct Tb { int b; Tb(int n) : b(n) {} };
struct Dt : Tb { Dt() : Tb(33) {} Dt(int n) : Tb(n) {} };
template<class T> int slice_by_value(T x, Tb b) { return b.b; }
template<class T> int slice_bare(T x, Tb b, int y) { return b.b * 10 + y; }
}
