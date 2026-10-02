// C++20 equivalent of std_20_span_bit_numbers.cb (compile-time parity baseline)
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <vector>

int main()
{
    int failures = 0;
    std::vector<int> v{3, 5, 7};
    std::span<int> sv(v);
    if (sv[1] != 5 || sv.subspan(1).size() != 2 || sv.first(1)[0] != 3 || sv.last(1)[0] != 7) failures |= 1;
    int fixed[3] = {2, 4, 6};
    std::span<int> sf(fixed);
    if (sf[2] != 6) failures |= 2;
    if (std::popcount(0x15u) != 3 || !std::has_single_bit(8u) || std::bit_width(9u) != 4 || std::rotl(1u, 1) != 2u) failures |= 4;
    float f = 1.5f;
    auto bits = std::bit_cast<std::uint32_t>(f);
    if (bits != 1069547520u) failures |= 8;
    int midpoint = std::midpoint(2, 8);
    double interpolated = std::lerp(2.0, 6.0, 0.5);
    if (midpoint != 5 || interpolated != 4.0) failures |= 16;
    if (std::ssize(v) != 3 || std::to_array("abc")[2] != 'c') failures |= 32;
    return failures;
}
