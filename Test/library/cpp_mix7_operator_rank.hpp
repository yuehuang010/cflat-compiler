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
    struct SFINAEFree
    {
        template<class T> int operator+(T*) const { return 1; }
    };
    inline int operator+(const SFINAEFree&, int) { return 2; }
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
    inline int spaceshipChoice = 0;
    struct D { int v; };
    inline int operator<=>(const D&, long) { spaceshipChoice = 83; return -1; }
    inline int unaryChoice = 0;
    struct U { int v; };
    inline int operator~(const U&) { unaryChoice = 71; return 71; }
    inline int rightChoice = 0;
    struct Right { int v; };
    inline int operator+(long, const Right&) { rightChoice = 11; return 11; }
}

inline int operator+=(mix7op::W& a, int) { a.v += 100; return a.v; }
inline int operator+=(mix7op::T& a, int) { a.v += 2; return a.v; }
inline int operator<=>(const mix7op::D&, int) { mix7op::spaceshipChoice = 84; return -1; }
namespace mix7op
{
    inline int scalarFirstSpaceshipChoice = 0;
    struct ScalarFirst { int v; };
    inline int operator<=>(long, const ScalarFirst&) { scalarFirstSpaceshipChoice = 183; return -1; }
    struct B2 {};
    struct L2 : B2 {};
    struct D2 { int v; };
    inline int classLeftSpaceshipChoice = 0;
    inline int operator<=>(const D2&, const B2&) { classLeftSpaceshipChoice = 83; return -1; }
}
inline int operator<=>(int, const mix7op::ScalarFirst&)
{
    mix7op::scalarFirstSpaceshipChoice = 184;
    return -1;
}
inline int operator<=>(const mix7op::D2&, const mix7op::L2&)
{
    mix7op::classLeftSpaceshipChoice = 84;
    return -1;
}
namespace mix7op
{
    struct CompoundBodyError
    {
        int v;
        template<class T> CompoundBodyError& operator+=(T value)
        {
            v = value.nope;
            return *this;
        }
    };
    inline CompoundBodyError& operator+=(CompoundBodyError& x, double)
    {
        x.v = 61;
        return x;
    }
    inline int rewrittenViableTemplateChoice = 0;
    struct RewrittenViableTemplate
    {
        template<class T> int operator<=>(T) const
        {
            rewrittenViableTemplateChoice = 1;
            return -1;
        }
    };
    inline int operator<=>(const RewrittenViableTemplate&, double)
    {
        rewrittenViableTemplateChoice = 2;
        return 1;
    }
}
inline int operator~(mix7op::U&)
{
    mix7op::unaryChoice = 72;
    return 72;
}
inline int operator+(int, const mix7op::Right&)
{
    mix7op::rightChoice = 12;
    return 12;
}
