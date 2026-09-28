#pragma once

// The SAME using-declarations as cpp_interop_usingns.h, repeated by a second, separately imported
// header (plus an overload next to the re-exported set). Re-declaring a using of the same entity
// is fine in C++, so the second import must bind silently, not read as a conflicting declaration.
#include "cpp_interop_usingns.h"
namespace cppu_decl_a
{
    using cppu_decl_b::EC;
    using cppu_decl_b::EU_A;
    using cppu_decl_b::decl_counter;
    using cppu_decl_b::ov;
    using cppu_decl_b::Box;
    using cppu_decl_b::TBox;
    inline int ov(double) noexcept { return 3; }
}
