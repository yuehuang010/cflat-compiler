// Global operator new/delete replacements for the unwind fixture: while countHeap is set every
// call is counted, so an allocation leaked or released twice by an unwind shows up.
#include <cstdlib>
#include <new>
#include "cpp_interop_unwind.h"
namespace cppunw {
int newCalls = 0;
int deleteCalls = 0;
int failNextNew = 0;
bool countHeap = false;
int guardedAlloc(Cb f, int x)
{
    try { return f(x); }
    catch (int e) { return -e; }
    catch (const std::bad_alloc&) { return -100; }
}
}
void* operator new(std::size_t n)
{
    if (cppunw::failNextNew > 0) { cppunw::failNextNew = 0; throw std::bad_alloc(); }
    if (cppunw::countHeap) ++cppunw::newCalls;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept
{
    if (p != nullptr && cppunw::countHeap) ++cppunw::deleteCalls;
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
