#include <string>
#include <cstdio>
#include <ranges>
int main()
{
    int sum = 0; for (int x : std::views::common(std::views::iota(1, 4))) sum += x;
    int plain_sum = 0; for (int x : std::views::iota(1, 4)) plain_sum += x;
    if (sum != 6 || plain_sum != 6) { std::puts("FAIL bounded iota view"); return 1; }
    std::puts("PASS std_full_20_901_common_iota"); return 0;
}
