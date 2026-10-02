// C++20 equivalent of std_11_memory_utility.cb (compile-time parity baseline)
#include <memory>
#include <tuple>
#include <utility>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::unique_ptr<int> p(new int(7));
    std::vector<std::unique_ptr<int>> v; v.emplace_back(new int(8)); if (*v[0] != 8) failures |= 2;
    auto shared = std::make_shared<int>(9); std::weak_ptr<int> w = shared; if (shared.use_count() != 1 || *w.lock() != 9) failures |= 4;
    std::pair<int, int> pr{2, 3}; auto tup = std::make_tuple(4, 5);
    int a = 1, b = 2; std::swap(a, b); int z = std::move(a); std::vector<int> em; em.emplace_back(std::forward<int>(z));
    if (*p != 7 || pr.first != 2 || pr.second != 3 || std::get<0>(tup) != 4 || std::get<1>(tup) != 5 || a != 2 || b != 1 || em[0] != 2) { std::printf("FAIL utility\n"); failures |= 8; }
    if (!failures) std::printf("PASS std_11_memory_utility\n");
    return failures;
}
