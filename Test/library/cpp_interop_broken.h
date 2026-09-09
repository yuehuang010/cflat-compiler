// Deliberately ill-formed C++ header for the bind-refusal error test. It (a) pulls in the STL
// and (b) declares a class clang rejects - `std::string` is never declared, since <string> is
// not included. Both halves matter: the invalid field cascades into every STL template that
// touches the record, which used to take the companion-CodeGen pass down with an access
// violation instead of reporting the diagnostic. Never bound by a passing test.
#pragma once

#include <vector>

namespace cppbroken
{
    struct Broken
    {
        std::string name;
        int get() const { return 1; }
    };

    inline std::vector<int> uses_stl() { return std::vector<int>(); }
}
