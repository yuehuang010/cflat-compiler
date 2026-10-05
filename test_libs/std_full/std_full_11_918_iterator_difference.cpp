// C++20 equivalent of std_full_11_918_iterator_difference.cb
#include <string>
#include <deque>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::vector<int> v{1, 2, 3};
    long d = v.end() - v.begin();
    if (d != 3) { std::printf("FAIL vector_iterator_difference\n"); failures |= 1; }
    std::deque<int> q{1, 2, 3, 4};
    long dq = q.end() - q.begin();
    if (dq != 4) { std::printf("FAIL deque_iterator_difference\n"); failures |= 2; }
    if (failures == 0) std::printf("PASS std_full_11_918_iterator_difference\n");
    return failures;
}
