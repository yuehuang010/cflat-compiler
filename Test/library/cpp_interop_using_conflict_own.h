#pragma once
#include "cpp_interop_using_conflict.h"
namespace cppuc
{
    enum class E { A = 1, B = 2 };
    inline int Red() noexcept { return 5; }
    struct S { long w; };
    template<class T> struct TT { long w; };
}
