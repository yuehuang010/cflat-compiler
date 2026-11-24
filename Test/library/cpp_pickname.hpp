#pragma once
// A CFlat method sharing a C++ member's simple name must not change the C++ member pick.
namespace pickname {
struct NB { int b = 1; };
struct ND : NB { int d = 2; };
inline ND ndobj;
inline ND* nd_ptr() { return &ndobj; }
struct NSink {
    int pickAlias(const NB*) const { return 1; }
    int pickAlias(const void*) const { return 2; }
};
struct MSink {
    int pickAlias(const NB*) { return 1; }
    int pickAlias(const void*) { return 2; }
};
template<class T> struct TSink {
    int pickAlias(const NB*) const { return 1; }
    int pickAlias(const void*) const { return 2; }
};
struct DSink : NSink { };
// clang++ -std=c++20: derived-to-base pointer conversion beats conversion to void*.
inline int oraclePick() { NSink s; return s.pickAlias(nd_ptr()); }
}
