#pragma once
#include "cpp_t480_ternleft4.hpp"

namespace cppbv {
inline TernaryCnt* ternleft4PointerSlot = nullptr;
inline TernaryCnt*& gpref() noexcept { return ternleft4PointerSlot; }
}
