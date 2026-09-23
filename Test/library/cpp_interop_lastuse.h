#pragma once

#include "cpp_interop_twin.h"

namespace cpptw
{
    inline int take(Twin value) { return value.value(); }
    inline int take2(Twin first, Twin second) { return first.value() + second.value(); }
    inline int t = 30;
    inline int nested_global() { return t; }
    inline int packed_counts() { return copy_count() * 100 + move_count() * 10 + dtor_count(); }
}
