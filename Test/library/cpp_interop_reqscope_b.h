// Order-independence fixture, part B. `leaked()` exists only when part A's macro is visible, so a
// request that compiled both headers in one translation unit would bind it.
#pragma once

namespace reqb {

template <class T>
struct BHolder
{
    T value;
    T get() const noexcept { return value; }
    void set(T v) noexcept { value = v; }
#ifdef CFLAT_REQ_SCOPE_LEAK
    int leaked() const noexcept { return 1; }
#endif
};

}
