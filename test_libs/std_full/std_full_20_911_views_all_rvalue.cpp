#include <ranges>
#include <utility>
#include <vector>
#include <cstdio>
int main()
{
    auto view = std::views::all(std::vector<int>{1, 2, 3});
    if (view.size() != 3) { std::puts("FAIL views_all_rvalue"); return 1; }
    std::puts("PASS std_full_20_911_views_all_rvalue");
    return 0;
}
