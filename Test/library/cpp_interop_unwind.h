#pragma once
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
inline int safe(int v) noexcept { return v + 1; }
typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
// Also catches std::bad_alloc (as -100).
int guardedAlloc(Cb f, int x);
}
