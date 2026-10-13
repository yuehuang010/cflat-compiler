// A private base with a field, a static, a member function, and operators. Named through an
// instance of the derived class, every member is inaccessible: the refusal names the base.
#pragma once
namespace cppi_pbi
{
    struct PbiBase
    {
        int v = 1;
        PbiBase() = default;
        PbiBase(int n) : v(n) {}
        static int K() { return 11; }
        int get() const { return v; }
        PbiBase operator+(const PbiBase& rhs) const { return PbiBase(v + rhs.v); }
        PbiBase operator-() const { return PbiBase(-v); }
    };
    struct PbiPriv : private PbiBase { PbiPriv() = default; PbiPriv(int n) : PbiBase(n) {} };
}
