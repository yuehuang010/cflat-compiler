#include <string>
#include <cstdio>
#include <ranges>
#include <vector>
int main()
{
    std::vector<int> values{1, 2};
    auto reversed = std::views::reverse(values);
    if (*reversed.begin() != 2) { std::puts("FAIL reverse view first"); return 1; }
    std::puts("PASS std_full_20_910_reverse_view"); return 0;
}
