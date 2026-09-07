// Second import GROUP over the same header-only C++ code (it includes cpp_interop_inline.h), so
// two companion modules emit the SAME inline bodies and vtables. Linked together they must merge
// by ODR (linkonce_odr + COMDAT), not collide as duplicate symbols - that is what this fixture
// exists to prove. It adds one inline function of its own so the group is not identical bitcode.
#pragma once

#include "cpp_interop_inline.h"

namespace cppinl2
{
    inline int twice_plus(int v) noexcept { return cppinl::twice(v) + 1; }
}
