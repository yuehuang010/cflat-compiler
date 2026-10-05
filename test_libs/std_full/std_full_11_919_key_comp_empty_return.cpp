// C++20 equivalent of std_full_11_919_key_comp_empty_return.cb
#include <string>
#include <map>
#include <set>
#include <cstdio>

int main() {
    int failures = 0;
    std::set<int> s{1, 2};
    auto kc = s.key_comp();
    if (!kc(1, 2) || kc(2, 1)) { std::printf("FAIL set_key_comp\n"); failures |= 1; }
    std::map<int, int> m{{1, 1}, {2, 2}};
    auto mkc = m.key_comp();
    if (!mkc(1, 2)) { std::printf("FAIL map_key_comp\n"); failures |= 2; }
    if (failures == 0) std::printf("PASS std_full_11_919_key_comp_empty_return\n");
    return failures;
}
