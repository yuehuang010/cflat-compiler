// C++20 equivalent of std_20_ranges_concepts.cb (compile-time parity baseline)
#include <algorithm>
#include <concepts>
#include <ranges>
#include <string>
#include <vector>

int main()
{
    int failures = 0;
    std::vector<int> v{5, 1, 4, 2, 3};
    std::ranges::sort(v);
    auto found = std::ranges::find(v, 4);
    if (v[0] != 1 || found == v.end() || *found != 4) failures |= 1;
    std::vector<int> data{5, 1, 4, 2, 3};
    std::ranges::sort(data);
    auto ptr = std::ranges::data(data);
    if (ptr[1] != 2) failures |= 2;
    return failures;
}
