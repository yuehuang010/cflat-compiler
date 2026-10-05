#include <string>
#include <cstdio>
#include <ranges>
#include <map>
#include <utility>
#include <vector>
int main()
{
    std::vector<std::pair<int, int>> values{{1, 2}};
    int sum = 0; for (int x : values | std::views::elements<1>) sum += x;
    std::map<int, int> mapping{{1, 10}, {2, 20}};
    int key_sum = 0, value_sum = 0;
    for (int x : mapping | std::views::keys) key_sum += x;
    for (int x : mapping | std::views::values) value_sum += x;
    if (!(sum == 2 && key_sum == 3 && value_sum == 30)) { std::puts("FAIL elements/keys/values views"); return 1; }
    std::puts("PASS std_full_20_904_elements_view"); return 0;
}
