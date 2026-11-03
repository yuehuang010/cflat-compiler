#pragma once
// Primary variable templates. Imported on its own line BEFORE cpp_cppfix_spec.hpp, which
// supplies the explicit and partial specializations (a later import must still win).
namespace cppfix
{
    template<class T> inline constexpr int V = 3;
    template<class T> inline constexpr int W = 1;
}
