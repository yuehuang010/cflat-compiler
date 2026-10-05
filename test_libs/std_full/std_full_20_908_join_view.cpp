#include <string>
#include <cstdio>
#include <ranges>
#include <vector>
int main()
{
    std::vector<std::vector<int>> values{{4}};
    int sum = 0; for (int x : std::views::join(values)) sum += x;
    if (sum != 4) { std::puts("FAIL join view"); return 1; }
    std::puts("PASS std_full_20_908_join_view"); return 0;
}
