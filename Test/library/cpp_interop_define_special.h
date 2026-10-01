#pragma once

#include "cpp_interop_define_primary.h"

#ifdef CFLAT_DEFINE_SPECIAL
namespace cppd
{
    template<> struct Layout<long> { long a; long b; };
    template<class T> struct PointerLayout<T*> { T* a; long b; };
}
#endif
