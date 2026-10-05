#include <string>
#include <cstdio>
#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <ranges>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>
int main()
{
    int failures = 0;
    std::vector<int> values{5, 1, 4, 2, 3};
    std::ranges::sort(values);
    std::array<int, 5> copied{};
    std::ranges::copy(values, copied.begin());
    std::identity idf{};
    int count = std::ranges::count(values, 3, idf);
    auto mm = std::ranges::minmax(values);
    bool binary = std::ranges::binary_search(values, 4, {}, std::identity{});
    std::vector<int> merged;
    std::ranges::set_union(std::vector<int>{1, 3, 5}, std::vector<int>{2, 3, 4}, std::back_inserter(merged));
    if (copied[0] != 1 || copied[4] != 5 || count != 1 || mm.min != 1 || mm.max != 5 || !binary || merged.size() != 5 || merged[2] != 3) failures |= 1;

    int single_sum = 0; for (int x : std::views::single(7)) single_sum += x;
    if (single_sum != 7) failures |= 2;

    std::vector<std::vector<int>> nested{{1, 2}, {3}, {4, 5}};
    std::string_view csv = "aa,bb,cc";
    int split_count = 0; for (auto part : std::views::split(csv, ',')) { if (part.size() == 2) split_count++; }
    int lazy_count = 0; for (auto part : std::views::lazy_split(csv, ',')) { if (std::ranges::distance(part) == 2) lazy_count++; }
    int counted_sum = 0; int raw[] = {2, 4, 6}; for (int x : std::views::counted(raw, 3)) counted_sum += x;
    if (split_count != 3 || lazy_count != 3 || counted_sum != 12) failures |= 4;

    std::map<int, int> mapping{{1, 10}, {2, 20}};
    int key_sum = 0, value_sum = 0;
    for (int x : mapping | std::views::keys) key_sum += x;
    for (int x : mapping | std::views::values) value_sum += x;
    auto sub = std::ranges::subrange(values.begin() + 1, values.end() - 1);
    int sub_sum = 0; for (int x : sub) sub_sum += x;
    auto ref = std::views::all(values);
    auto own = std::views::all(std::vector<int>{8, 9});
    if (key_sum != 3 || value_sum != 30 || sub_sum != 9 || ref.size() != 5 || own.size() != 2) failures |= 8;
    if (failures & 1) std::puts("FAIL ranges algorithms/projection");
    if (failures & 2) std::puts("FAIL basic views");
    if (failures & 4) std::puts("FAIL range adaptor views");
    if (failures & 8) std::puts("FAIL subrange and views");
    if (failures == 0) std::puts("PASS std_full_20_ranges");
    return failures;
}
