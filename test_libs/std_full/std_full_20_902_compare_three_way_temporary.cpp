// C++20 equivalent of std_full_20_902_compare_three_way_temporary.cb
#include <string>
#include <compare>
#include <cstdio>

int main() {
    int failures = 0;
    auto c1 = std::compare_three_way()(1, 2);
    auto c2 = std::compare_three_way()(2, 2);
    if (!std::is_lt(c1) || !std::is_eq(c2)) { std::printf("FAIL compare_three_way_temporary\n"); failures |= 1; }
    if (failures == 0) std::printf("PASS std_full_20_902_compare_three_way_temporary\n");
    return failures;
}
