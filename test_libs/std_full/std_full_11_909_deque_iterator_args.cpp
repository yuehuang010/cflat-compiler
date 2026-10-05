// C++20 equivalent of std_full_11_909_deque_iterator_args.cb
#include <string>
#include <deque>
#include <cstdio>

int main() {
    int failures = 0;
    std::deque<int> d{1, 2, 3, 4};
    d.insert(d.begin() + 2, 99);
    if (d.size() != 5 || d[2] != 99) { std::printf("FAIL deque_insert\n"); failures |= 1; }
    d.erase(d.begin() + 2);
    if (d.size() != 4 || d[2] != 3) { std::printf("FAIL deque_erase\n"); failures |= 2; }
    auto rb = d.rbegin();
    if (*rb != 4) { std::printf("FAIL deque_rbegin\n"); failures |= 4; }
    if (failures == 0) std::printf("PASS std_full_11_909_deque_iterator_args\n");
    return failures;
}
