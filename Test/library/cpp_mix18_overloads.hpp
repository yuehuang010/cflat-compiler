#pragma once

// MIX18: a free function taking by-value long / float / unsigned beside a const double& takes an int
// by value (CFlat ruling: a converted temporary ranks below a by-value bind; clang reports these
// pairs ambiguous). A const double& beside a const long& ties (clang: ambiguous). CBI: a function
// value into a bool constructor beside an int one is the bool one. CB: a const bool& constructor
// takes a function value or a pointer as its truth.
namespace m18ov
{
    inline int a1(const double&) { return 1; }
    inline int a1(long) { return 2; }
    inline int a2(const double&) { return 1; }
    inline int a2(float) { return 2; }
    inline int a7(const double&) { return 1; }
    inline int a7(const long&) { return 2; }
    inline int a9(const double&) { return 1; }
    inline int a9(int) { return 2; }
    inline int a11(const double&) { return 1; }
    inline int a11(unsigned) { return 2; }
    struct CBI { int v; CBI(bool b) : v(b ? 1 : 2) {} CBI(int x) : v(x + 100) {} };
    struct CB { int v; CB(const bool& b) : v(b ? 1 : 2) {} };
    // MIX18 r2: the same const double& + long tie on a constructor, a method and an operator (by-value wins).
    struct TieS
    {
        int v;
        TieS(const double&) : v(1) {}
        TieS(long) : v(2) {}
        int m(const double&) { return 1; }
        int m(long) { return 2; }
        int operator+(const double&) { return 1; }
        int operator+(long) { return 2; }
    };
    struct TieU { int v; TieU(const long&) : v(1) {} TieU(double) : v(2) {} };
    struct TieT { int v; TieT(const double&) : v(1) {} TieT(const float&) : v(2) {} };
    // MIX18 r2: a CFlat function value into a free `const bool&` is its truth; an int takes the int overload.
    inline int a3(const bool&) { return 1; }
    inline int a3(int) { return 2; }
    // MIX18 r3: an identity bind (same scalar type) is never a converted temporary: master picks the const& (1).
    inline int gi(const int&) { return 1; }
    inline int gi(int) { return 2; }
    inline int gc(const char&) { return 1; }
    inline int gc(char) { return 2; }
    inline int gs(const short&) { return 1; }
    inline int gs(short) { return 2; }
    inline int gll(const long long&) { return 1; }
    inline int gll(long long) { return 2; }
}
