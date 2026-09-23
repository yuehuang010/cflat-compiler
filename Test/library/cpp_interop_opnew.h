#pragma once
// Class-scope allocation functions (legs 3350-3366). Every class-level operator new / delete /
// new[] / delete[] counts its calls, so an allocation that bypasses it (the global operator) or a
// free through the wrong family shows up as an exact count. The global replacements that count
// ::operator new[] / delete[] live in cpp_interop_unwind.cpp.
#include <cstddef>
#include <cstdlib>
#include <cstring>
namespace cppon {
struct Cnt
{
    inline static int clsNew = 0, clsDel = 0, clsNewArr = 0, clsDelArr = 0, live = 0, dtors = 0;
};
// No class-level operators: the global ones apply.
struct Plain
{
    int v;
    Plain() : v(7) { ++Cnt::live; }
    ~Plain() { --Cnt::live; ++Cnt::dtors; }
    int get() const { return v; }
};
// Class-level operator new / delete (unsized); `new Cls[n]` still uses ::operator new[].
struct Cls
{
    int v;
    Cls() : v(7) { ++Cnt::live; }
    Cls(int x) : v(x) { if (x < 0) throw -x; ++Cnt::live; }
    ~Cls() { --Cnt::live; ++Cnt::dtors; }
    int get() const { return v; }
    static void* operator new(std::size_t n) { ++Cnt::clsNew; return std::malloc(n); }
    static void operator delete(void* p) { ++Cnt::clsDel; std::free(p); }
};
// Inherits Cls's operators through name lookup.
struct ClsDerived : Cls { ClsDerived(int x) : Cls(x) {} };
// Only the SIZED class delete: the size must be sizeof(Sized), else the count jumps by 100.
struct Sized
{
    int v;
    long long pad;
    Sized(int x) : v(x), pad(0) { ++Cnt::live; }
    ~Sized() { --Cnt::live; ++Cnt::dtors; }
    int get() const { return v; }
    static void* operator new(std::size_t n) { ++Cnt::clsNew; return std::malloc(n); }
    static void operator delete(void* p, std::size_t n)
    { Cnt::clsDel += n == sizeof(Sized) ? 1 : 100; std::free(p); }
};
// Class-level array operators; the sized delete[] checks the size is a whole number of elements.
struct Arr
{
    int v;
    Arr() : v(3) { ++Cnt::live; }
    ~Arr() { --Cnt::live; ++Cnt::dtors; }
    int get() const { return v; }
    static void* operator new(std::size_t n) { ++Cnt::clsNew; return std::malloc(n); }
    static void operator delete(void* p) { ++Cnt::clsDel; std::free(p); }
    static void* operator new[](std::size_t n) { ++Cnt::clsNewArr; return std::malloc(n); }
    static void operator delete[](void* p, std::size_t n)
    { Cnt::clsDelArr += n % sizeof(Arr) == 0 ? 1 : 100; std::free(p); }
};
// Polymorphic: `delete base` goes through the deleting destructor, which calls Poly's delete.
struct Poly
{
    int v;
    Poly() : v(1) { ++Cnt::live; }
    Poly(int x) : v(x) { ++Cnt::live; }
    virtual ~Poly() { --Cnt::live; ++Cnt::dtors; }
    virtual int get() const { return v; }
    static void* operator new(std::size_t n) { ++Cnt::clsNew; return std::malloc(n); }
    static void operator delete(void* p) { ++Cnt::clsDel; std::free(p); }
};
struct PolyDerived : Poly
{
    PolyDerived(int x) : Poly(x) {}
    ~PolyDerived() override { ++Cnt::dtors; }
    int get() const override { return v * 2; }
};
// Trivial (no user constructor) with class operators; the class new POISONS the block, so a
// field reading 0 proves CFlat's zeroed `new` storage survived the switch to the class new.
struct Triv
{
    int a;
    int b;
    static void* operator new(std::size_t n)
    { ++Cnt::clsNew; void* p = std::malloc(n); std::memset(p, 0x5a, n); return p; }
    static void operator delete(void* p) { ++Cnt::clsDel; std::free(p); }
};
// Over-aligned, no class operators: the aligned global forms.
struct alignas(64) Wide
{
    int v;
    Wide() : v(6) { ++Cnt::live; }
    ~Wide() { --Cnt::live; ++Cnt::dtors; }
    int get() const { return v; }
};
inline void reset() noexcept
{ Cnt::clsNew = Cnt::clsDel = Cnt::clsNewArr = Cnt::clsDelArr = Cnt::live = Cnt::dtors = 0; }
inline int clsNew() noexcept { return Cnt::clsNew; }
inline int clsDel() noexcept { return Cnt::clsDel; }
inline int clsNewArr() noexcept { return Cnt::clsNewArr; }
inline int clsDelArr() noexcept { return Cnt::clsDelArr; }
inline int live() noexcept { return Cnt::live; }
inline int dtors() noexcept { return Cnt::dtors; }
}
