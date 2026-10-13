// Two overloads that differ only in a defaulted reference parameter: each candidate line of an
// ambiguous call shows the defaults of its reference parameters ("= ...").
#pragma once
#include <optional>
namespace cppi_rdt
{
    inline int pick(int, const int& x = 1, std::optional<int> z = {}) { return 21; }
    inline int pick(int, const long& x = 2, std::optional<int> z = {}) { return 22; }
}
