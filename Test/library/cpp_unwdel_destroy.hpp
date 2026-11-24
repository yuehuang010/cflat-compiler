#pragma once
// UNWDEL r2b: C++20 destroying delete beats a sized (void*, long long) delete. Each delete form
// bumps its own counter so a leg can tell which one the deallocation picked.
#include <new>
#include <cstdlib>

namespace unwdel_dd {
inline int destroyCalls = 0;
inline int sizedCalls = 0;
}

struct UnwdelDestroy {
    int z = 0;
    UnwdelDestroy(int x) : z(x) {}
    static void operator delete(UnwdelDestroy* p, std::destroying_delete_t) {
        ++unwdel_dd::destroyCalls;
        p->~UnwdelDestroy();
        ::operator delete(p);
    }
    static void operator delete(void* p, long long) {
        ++unwdel_dd::sizedCalls;
        std::free(p);
    }
};

inline void unwdelDdReset() { unwdel_dd::destroyCalls = 0; unwdel_dd::sizedCalls = 0; }
inline int unwdelDdDestroyCount() { return unwdel_dd::destroyCalls; }
inline int unwdelDdSizedCount() { return unwdel_dd::sizedCalls; }
