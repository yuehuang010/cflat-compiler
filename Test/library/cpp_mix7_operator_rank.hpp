#pragma once

// MIX7: a namespace operator and a global operator are one C++ candidate set (ordinary lookup
// plus ADL). The namespace operator is found by ADL, the global one by ordinary lookup.
namespace mix7op
{
    struct Y { int v; };
    inline int operator+(const Y&, long) { return 11; }
    struct Z { int v; };
    inline int operator+(const Z&, int) { return 21; }
    struct X { int v; };
    inline int operator+(const X&, int) { return 1; }
}

inline int operator+(const mix7op::Y&, int) { return 12; }
inline int operator+(const mix7op::Z&, long) { return 22; }
inline int operator+(const mix7op::X&, int) { return 2; }

// Compound assignment: a namespace `operator+=` and a global one are one candidate set too.
namespace mix7op
{
    struct W { int v; };
    inline int operator+=(W& a, long) { a.v += 1; return a.v; }
    struct T { int v; };
    inline int operator+=(T& a, int) { a.v += 1; return a.v; }
}

inline int operator+=(mix7op::W& a, int) { a.v += 100; return a.v; }
inline int operator+=(mix7op::T& a, int) { a.v += 2; return a.v; }
