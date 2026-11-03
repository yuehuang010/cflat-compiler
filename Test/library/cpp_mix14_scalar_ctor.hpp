#pragma once

// MIX14: a constructor or function taking const T& of another scalar type binds a converted
// temporary for an int argument (clang: DR 8, BR 1, FR 16). DRV: an exact int constructor beside
// the const double& one is the overload clang picks for an int (104). OB: a CFlat function pointer
// passed to a C++ bool constructor parameter is its truth (set 1, null 2). CI: an int by-value ctor
// refuses a function (pointer -> integer). CIB: int and bool ctors beside each other.
namespace mix14sc
{
    struct DR { int v; DR(const double& d) : v((int)(d * 2)) {} };
    struct BR { int v; BR(const bool& b) : v(b ? 1 : 2) {} };
    struct FR { int v; FR(const float& d) : v((int)(d * 4)) {} };
    struct DRV { int v; DRV(const double& d) : v((int)(d * 2)) {} DRV(int x) : v(x + 100) {} };
    struct OB { int v; OB(bool b) : v(b ? 1 : 2) {} };
    struct CI { int v; CI(int x) : v(x) {} };
    struct CIB { int v; CIB(int x) : v(x + 100) {} CIB(bool b) : v(b ? 1 : 2) {} };
    inline int fdr(const double& d) { return (int)(d * 2); }
    inline int fbr(const bool& b) { return b ? 1 : 2; }
    inline int fff(const float& d) { return (int)(d * 4); }
}
