// C++20 equivalent of std_20_containers_strings.cb (compile-time parity baseline)
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <functional>

static bool is_four(int item) { return item == 4; }
static bool is_five(int item) { return item == 5; }

int main()
{
    int failures = 0;
    std::map<int, int> m{{1, 10}, {2, 20}};
    std::set<int> s{4, 8};
    std::unordered_set<int> us{5, 9};
    if (!m.contains(2) || !s.contains(8) || !us.contains(9)) failures |= 1;
    std::vector<int> erase_values{2, 3};
    if (std::erase(erase_values, 2) != 1 || erase_values.size() != 1) failures |= 2;
    std::set<int> eif_ordered{4, 8};
    std::unordered_set<int> eif_hashed{5, 9};
    int erased_ordered = std::erase_if(eif_ordered, std::function<bool(int)>(is_four));
    int erased_hashed = std::erase_if(eif_hashed, std::function<bool(int)>(is_five));
    if (erased_ordered != 1 || erased_hashed != 1 || eif_ordered.size() != 1 || eif_hashed.size() != 1 || eif_ordered.contains(4) || !eif_ordered.contains(8) || eif_hashed.contains(5) || !eif_hashed.contains(9)) failures |= 16;
    std::string text = "prefix-middle-suffix";
    std::string_view view = "prefix-middle-suffix";
    if (!text.starts_with("prefix") || !text.ends_with("suffix") || !view.starts_with("prefix") || !view.ends_with("suffix")) failures |= 4;
    if (!(std::pair{1, 2} < std::pair{1, 3}) || !(std::string("a") < std::string("b")) || !(std::vector<int>{1, 2} < std::vector<int>{1, 3})) failures |= 8;
    return failures;
}
