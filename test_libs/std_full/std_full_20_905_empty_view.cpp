#include <string>
#include <cstdio>
#include <ranges>
int main()
{
    int count = 0;
    for (int x : std::views::empty<int>) count += x;
    if (count != 0) { std::puts("FAIL empty view count"); return 1; }
    std::puts("PASS std_full_20_905_empty_view"); return 0;
}
