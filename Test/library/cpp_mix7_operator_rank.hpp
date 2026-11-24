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

// OPRANK: a member operator template beside a free candidate is one C++ set that clang ranks.
// Codes: 11 member template, 22 free non-template, 33 free template, 44 member non-template.
#include <type_traits>
namespace oprank
{
    inline int who = 0;
    template<class T> struct Never { static constexpr bool value = false; };
    // Accept-set (master already matched clang): binary, reversed, unary, class template.
    struct BinExact { template<class T> int operator+(T) const { return 11; } };
    inline int operator+(const BinExact&, int) { return 22; }
    struct BinSfinae { template<class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
                       int operator+(T) const { return 11; } };
    inline int operator+(const BinSfinae&, double) { return 22; }
    struct BinBody { template<class T> int operator+(T) const { static_assert(Never<T>::value); return 11; } };
    inline int operator+(const BinBody&, int) { return 22; }
    struct BinReversed { template<class T> int operator+(T) const { return 11; } };
    inline int operator+(int, const BinReversed&) { return 22; }
    struct BinLong { template<class T> int operator+(T) const { return 11; } };
    inline int operator+(const BinLong&, long) { return 22; }
    template<class U> struct BinClassTemplate { template<class T> int operator+(T) const { return 11; } };
    inline int operator+(const BinClassTemplate<int>&, double) { return 22; }
    using BinClassTemplateInt = BinClassTemplate<int>;
    struct UnaryTemplate { protected: template<class T = int> int operator-() const { return 11; } };
    inline int operator-(const UnaryTemplate&) { return 22; }
    struct CompoundExact { int v = 0; template<class T> CompoundExact& operator+=(T) { v = 11; return *this; } };
    inline CompoundExact& operator+=(CompoundExact& c, int) { c.v = 22; return c; }
    struct CompoundAlone { int v = 0; template<class T> CompoundAlone& operator+=(T) { v = 11; return *this; } };
    struct CompoundFreeTemplate { int v = 0; CompoundFreeTemplate& operator+=(double) { v = 44; return *this; } };
    template<class T> CompoundFreeTemplate& operator+=(CompoundFreeTemplate& c, T) { c.v = 33; return c; }
    struct CompoundMinus { int v = 0; template<class T> CompoundMinus& operator-=(T) { v = 11; return *this; } };
    inline CompoundMinus& operator-=(CompoundMinus& c, double) { c.v = 22; return c; }
    struct LessExact { template<class T> int operator<=>(T x) const { who = 11; return 0 - (int)x; } };
    inline bool operator<(const LessExact&, int) { who = 22; return true; }
    struct LessSfinae { template<class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
                        int operator<=>(T x) const { who = 11; return 0 - (int)x; } };
    inline bool operator<(const LessSfinae&, double) { who = 22; return true; }
    struct LessBody { template<class T> int operator<=>(T) const { static_assert(Never<T>::value); return 0; } };
    inline bool operator<(const LessBody&, int) { who = 22; return true; }
    struct EqExact { template<class T> bool operator==(T) const { who = 11; return true; } };
    inline bool operator==(const EqExact&, int) { who = 22; return true; }
    struct EqReversed { template<class T> bool operator==(T) const { who = 11; return true; } };
    inline bool operator==(int, const EqReversed&) { who = 22; return true; }
    // Fixed cells (master disagreed with clang).
    struct CompoundViable { int v = 0; template<class T> CompoundViable& operator+=(T) { v = 11; return *this; } };
    inline CompoundViable& operator+=(CompoundViable& c, double) { c.v = 22; return c; }
    struct CompoundSfinae { int v = 0; template<class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
                            CompoundSfinae& operator+=(T) { v = 11; return *this; } };
    inline CompoundSfinae& operator+=(CompoundSfinae& c, double) { c.v = 22; return c; }
    struct CompoundRequires { int v = 0; template<class T> requires std::is_floating_point_v<T>
                              CompoundRequires& operator+=(T) { v = 11; return *this; } };
    inline CompoundRequires& operator+=(CompoundRequires& c, double) { c.v = 22; return c; }
    struct CompoundUnselectedBody { int v = 0;
        template<class T> CompoundUnselectedBody& operator+=(T) { static_assert(Never<T>::value); return *this; } };
    inline CompoundUnselectedBody& operator+=(CompoundUnselectedBody& c, int) { c.v = 22; return c; }
    struct LessViable { template<class T> int operator<=>(T x) const { who = 11; return 0 - (int)x; } };
    inline bool operator<(const LessViable&, double) { who = 22; return true; }
    struct SpaceshipFree { template<class T> int operator<=>(T x) const { who = 11; return 0 - (int)x; } };
    inline int operator<=>(const SpaceshipFree&, int) { who = 22; return -1; }
    struct GreaterEqualViable { template<class T> int operator<=>(T x) const { who = 11; return 0 - (int)x; } };
    inline bool operator>=(const GreaterEqualViable&, double) { who = 22; return true; }
    // A derived class: the inherited member template ranks with a free operator taking the base.
    struct TemplateBase { int v = 0;
        template<class T> int operator+(T) const { return 11; }
        template<class T> TemplateBase& operator+=(T) { v = 11; return *this; }
        template<class T> int operator<=>(T x) const { who = 11; return 0 - (int)x; } };
    inline int operator+(const TemplateBase&, double) { return 22; }
    inline TemplateBase& operator+=(TemplateBase& c, double) { c.v = 22; return c; }
    inline int operator<=>(const TemplateBase&, int) { who = 22; return -1; }
    struct TemplateDerived : TemplateBase {};
    struct UnaryBase { int v = 0; };
    inline int operator-(const UnaryBase&) { return 22; }
    struct UnaryDerivedTemplate : UnaryBase { template<class T = int> int operator-() const { return 11; } };
    struct UnaryDerivedProtected : UnaryBase { protected: template<class T = int> int operator-() const { return 11; } };
    // Refusals (Test/errors/err_cpp_nonpublic_member_operator_template.cb).
    struct PrivateBinary { private: template<class T> int operator+(T) const { return 11; } };
    inline int operator+(const PrivateBinary&, double) { return 22; }
    struct ProtectedBase { protected: template<class T> int operator+(T) const { return 11; } };
    inline int operator+(const ProtectedBase&, double) { return 22; }
    struct ProtectedDerived : ProtectedBase {};
    template<class U> struct ProtectedClassTemplate { protected: template<class T> int operator+(T) const { return 11; } };
    inline int operator+(const ProtectedClassTemplate<int>&, double) { return 22; }
    using ProtectedClassTemplateInt = ProtectedClassTemplate<int>;
    struct ProtectedCompound { int v = 0; protected: template<class T> ProtectedCompound& operator+=(T) { v = 11; return *this; } };
    inline ProtectedCompound& operator+=(ProtectedCompound& c, double) { c.v = 22; return c; }
    struct ProtectedSpaceship { protected: template<class T> int operator<=>(T x) const { return 0 - (int)x; } };
    inline bool operator<(const ProtectedSpaceship&, double) { return true; }
    struct ProtectedEq { protected: template<class T> bool operator==(T) const { return true; } };
    inline bool operator==(const ProtectedEq&, double) { return true; }
}
