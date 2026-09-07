// C++ fixture for the M3 by-value boundary: a NONTRIVIAL record passed by value. Clang arranges
// it indirectly with the CALLER owning the temp and running its destructor after the call - that
// construction/destruction sequencing is M4, so registering this declaration must be refused.
// A trivially copyable record by value is legal and lives in cpp_interop_basic.h.
// Kept out of that fixture so it stays bindable.
#pragma once

namespace cppbv
{
    // User-provided destructor: not trivially copyable, so not a raw-bytes value at the boundary.
    struct Owner
    {
        int* p;
        ~Owner();
    };
    int sum_owner(Owner o) noexcept;
}
