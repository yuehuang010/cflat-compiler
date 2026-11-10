#pragma once
#include <new>
// Instrumented fixture for a C++ exception unwinding THROUGH CFlat frames: every Tr
// construction and destruction is counted, so a skipped or doubled destructor shows up.
namespace cppunw {
struct Tr
{
    inline static int live = 0;
    inline static int dtors = 0;
    int tag;
    Tr() : tag(0) { ++live; }
    Tr(int t) : tag(t) { ++live; }
    Tr(const Tr& o) : tag(o.tag) { ++live; }
    Tr(Tr&& o) noexcept : tag(o.tag) { ++live; }
    Tr& operator=(const Tr& o) { tag = o.tag; return *this; }
    ~Tr() { --live; ++dtors; }
    int get() const noexcept { return tag; }
};
// Tx shares Tr's counters; its special members throw on demand. setArm(n) makes the n-th next
// default ctor, copy ctor, copy- or move-assignment throw 5 (then disarms); Tx(t) throws t
// when t > 0.
struct Tx
{
    inline static int arm = 0;
    int tag;
    static void fire() { if (arm == 1) { arm = 0; throw 5; } if (arm > 1) --arm; }
    Tx() : tag(0) { fire(); ++Tr::live; }
    Tx(int t) : tag(t) { if (t > 0) throw t; ++Tr::live; }
    Tx(const Tx& o) : tag(o.tag) { fire(); ++Tr::live; }
    Tx(Tx&& o) noexcept : tag(o.tag) { ++Tr::live; }
    Tx& operator=(const Tx& o) { fire(); tag = o.tag; return *this; }
    Tx& operator=(Tx&& o) { fire(); tag = o.tag; return *this; }
    ~Tx() { --Tr::live; ++Tr::dtors; }
    int get() const noexcept { return tag; }
};
struct NewArgOrder
{
    inline static int allocations = 0;
    inline static int deallocations = 0;
    inline static int born = 0;
    inline static int dtors = 0;
    inline static int picked = 0;
    int value;
    static void* operator new(std::size_t n) { ++allocations; return ::operator new(n); }
    static void operator delete(void* p) noexcept { ++deallocations; ::operator delete(p); }
    explicit NewArgOrder(int x) : value(x + 40) { ++picked; ++born; }
    explicit NewArgOrder(long x) : value((int)x + 70) { picked += 2; ++born; }
    ~NewArgOrder() { ++dtors; }
    static void resetCounts() noexcept
    {
        allocations = deallocations = born = dtors = picked = 0;
    }
};
// Instrumented moved temporary used by nested aggregate replacement unwinds.
struct FreshReplace
{
    inline static int born = 0;
    inline static int dead = 0;
    inline static int moves = 0;
    inline static bool throwOld = false;
    int tag;
    FreshReplace(int t) : tag(t) { ++born; }
    FreshReplace(const FreshReplace&) = delete;
    FreshReplace(FreshReplace&& other) noexcept : tag(other.tag)
    { ++born; ++moves; other.tag = 0; }
    FreshReplace& operator=(FreshReplace&& other) noexcept
    { tag = other.tag; other.tag = 0; return *this; }
    ~FreshReplace() noexcept(false)
    {
        ++dead;
        if (tag == 1 && !throwOld) { throwOld = true; throw 71; }
    }
    static void reset() noexcept { born = dead = moves = 0; throwOld = false; }
};
inline int freshReplaceBorn() noexcept { return FreshReplace::born; }
inline int freshReplaceDead() noexcept { return FreshReplace::dead; }
inline int freshReplaceMoves() noexcept { return FreshReplace::moves; }
inline void resetFreshReplace() noexcept { FreshReplace::reset(); }
struct FreshReplaceArrayItem { FreshReplace value = FreshReplace(1); };
struct FreshReplaceArrayOwner { FreshReplaceArrayItem arr[2]; };
inline bool freshReplaceArrayOracle()
{
    FreshReplace::reset();
    alignas(FreshReplace) unsigned char storage[sizeof(FreshReplace) * 2];
    auto* arr = reinterpret_cast<FreshReplace*>(storage);
    new (&arr[0]) FreshReplace(1);
    new (&arr[1]) FreshReplace(1);
    try
    {
        FreshReplace rhs(2);
        arr[0].~FreshReplace();
    }
    catch (int) { arr[1].~FreshReplace(); }
    return FreshReplace::born == 3 && FreshReplace::dead == 3;
}
inline void setArm(int v) noexcept { Tx::arm = v; }
// The global operator new/delete and new[]/delete[] replacements in cpp_interop_unwind.cpp count
// calls while counting is on; failNew(1) makes the next operator new throw std::bad_alloc.
extern int newCalls;
extern int deleteCalls;
extern int newArrCalls;
extern int deleteArrCalls;
extern int failNextNew;
extern bool countHeap;
inline void failNew(int v) noexcept { failNextNew = v; }
inline int news() noexcept { return newCalls; }
inline int deletes() noexcept { return deleteCalls; }
inline int newArrs() noexcept { return newArrCalls; }
inline int deleteArrs() noexcept { return deleteArrCalls; }
inline void reset() noexcept
{
    Tr::live = 0; Tr::dtors = 0; Tx::arm = 0;
    newCalls = 0; deleteCalls = 0; newArrCalls = 0; deleteArrCalls = 0; failNextNew = 0;
    countHeap = true;
}
inline int live() noexcept { return Tr::live; }
inline int dtors() noexcept { return Tr::dtors; }
inline int thrower(int v) { if (v > 0) throw v; return v; }
inline Tr makeTr(int v) { if (v > 0) throw v; return Tr(v); }
inline int safe(int v) noexcept { return v + 1; }
typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
inline int newArgOrderBody(int x)
{
    NewArgOrder* p = new NewArgOrder(thrower(x));
    int value = p->value;
    delete p;
    return value;
}
inline int newArgOrderOracle(int x) { return guarded(newArgOrderBody, x); }
// Callbacks returning a struct reach CFlat through a reverse ABI thunk (16 bytes in registers,
// 24 bytes through sret); a throw from the CFlat body must unwind through that thunk.
struct S16 { long long a; long long b; };
struct S24 { long long a; long long b; long long c; };
typedef S16 (*Cb16)(int);
typedef S24 (*Cb24)(int);
inline int guarded16(Cb16 f, int x) { try { return (int)f(x).a; } catch (int e) { return -e; } }
inline int guarded24(Cb24 f, int x) { try { return (int)f(x).a; } catch (int e) { return -e; } }
// Also catches std::bad_alloc (as -100).
int guardedAlloc(Cb f, int x);
}
