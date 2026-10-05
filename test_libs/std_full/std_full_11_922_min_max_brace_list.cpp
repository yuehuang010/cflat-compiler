// C++20 equivalent of std_full_11_922_min_max_brace_list.cb
#include <string>
#include <algorithm>
#include <cstdio>

int main() {
    int failures = 0;
    auto mx = std::max({3, 9, 4});
    auto mn = std::min({3, 9, 4});
    auto mm = std::minmax({3, 9, 4});
    if (mx != 9 || mn != 3 || mm.first != 3 || mm.second != 9) { std::printf("FAIL min_max_brace_list\n"); failures |= 1; }
    if (failures == 0) std::printf("PASS std_full_11_922_min_max_brace_list\n");
    return failures;
}
