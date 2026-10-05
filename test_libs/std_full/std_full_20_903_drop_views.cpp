#include <string>
#include <cstdio>
#include <ranges>
#include <vector>
int main()
{
    std::vector<int> values{1, 2, 3};
    auto drop = std::views::drop(values, 1);
    auto drop_while = std::views::drop_while(values, [](int x) { return x < 2; });
    auto take_while = std::views::take_while(values, [](int x) { return x < 2; });
    if (!(*drop.begin() == 2 && *drop_while.begin() == 2 && *take_while.begin() == 1)) { std::puts("FAIL drop view begins"); return 1; }
    std::puts("PASS std_full_20_903_drop_views"); return 0;
}
