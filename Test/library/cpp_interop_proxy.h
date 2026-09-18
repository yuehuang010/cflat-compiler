// A user proxy class returned by a container's operator[], plus conversion-operator
// variants (implicit bool, explicit bool, implicit int), for the implicit-conversion path.
#pragma once

struct BitRef
{
    unsigned char* slot;
    operator bool() const { return *slot != 0; }
    BitRef& operator=(bool v) { *slot = v ? 1 : 0; return *this; }
};

struct BitBag
{
    unsigned char data[4];
    BitBag() { data[0] = 1; data[1] = 0; data[2] = 1; data[3] = 0; }
    BitRef operator[](int i) { BitRef r; r.slot = &data[i]; return r; }
    bool at(int i) const { return data[i] != 0; }
};

struct ExplicitBool
{
    int v;
    ExplicitBool() : v(0) {}
    ExplicitBool(int x) : v(x) {}
    explicit operator bool() const { return v != 0; }
};

// Identical to ExplicitBool except for the `explicit` keyword: the discriminator for
// Test/errors/err_cpp_proxy_conversion.cb's legs.
struct ImplicitBool
{
    int v;
    ImplicitBool() : v(0) {}
    ImplicitBool(int x) : v(x) {}
    operator bool() const { return v != 0; }
};

struct IntLike
{
    int v;
    IntLike() : v(0) {}
    IntLike(int x) : v(x) {}
    operator int() const { return v; }
};

inline int proxy_take_bool(bool b) { return b ? 7 : 3; }
inline int proxy_take_int(int i) { return i + 100; }
inline double proxy_take_double(double d) { return d + 0.5; }

// --- Overload ranking. A user-defined conversion sequence loses to every standard one, and
// among two sequences using the SAME operator the better second standard conversion wins.
struct Conv { int v; Conv() : v(0) {} Conv(int x) : v(x) {} operator int() const { return v; } };
inline int ovl_a(int x)          { return 1; }
inline int ovl_a(const Conv& c)  { return 2; }
inline int ovl_b(int x)    { return 10; }
inline int ovl_b(double d) { return 20; }
struct Both { int v; Both() : v(1) {} operator int() const { return 0; } operator bool() const { return true; } };
inline int ovl_c(bool b) { return b ? 30 : 31; }
// Each overload is reached through a DIFFERENT conversion operator of Both: ambiguous.
inline int ovl_two(int x)  { return 1; }
inline int ovl_two(bool b) { return 2; }

struct Base2 { int b; Base2() : b(9) {} };
struct Der2 : Base2 { operator int() const { return 3; } };
inline int ovl_f(const Base2& x) { return 100 + x.b; }
inline int ovl_f(int v)          { return 200 + v; }
// Same pair, declarations SWAPPED: the answer must not depend on header order.
struct Base3 { int b; Base3() : b(9) {} };
struct Der3 : Base3 { operator int() const { return 3; } };
inline int ovl_g(int v)          { return 200 + v; }
inline int ovl_g(const Base3& x) { return 100 + x.b; }

// --- Lifetime. Every conversion site materializes a proxy temporary that must be destroyed.
inline int life_ctor_n = 0;
inline int life_dtor_n = 0;
struct LifeRef
{
    unsigned char* slot;
    LifeRef(unsigned char* s) : slot(s) { ++life_ctor_n; }
    LifeRef(const LifeRef& o) : slot(o.slot) { ++life_ctor_n; }
    ~LifeRef() { ++life_dtor_n; }
    operator bool() const { return *slot != 0; }
};
struct LifeBag
{
    unsigned char data[4];
    LifeBag() { data[0] = 1; data[1] = 0; data[2] = 1; data[3] = 0; }
    LifeRef operator[](int i) { return LifeRef(&data[i]); }
};
inline int life_ctor() { return life_ctor_n; }
inline int life_dtor() { return life_dtor_n; }
inline int life_take_bool(bool b) { return b ? 1 : 0; }
