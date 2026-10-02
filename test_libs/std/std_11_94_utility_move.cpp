// C++20 equivalent of std_11_94_utility_move.cb (compile-time parity baseline)
#include <memory>
#include <utility>
#include <cstdio>
#include <string>

int main() {
    int failures = 0;
    std::unique_ptr<int> p(new int(7));
    std::unique_ptr<int> moved = std::move(p);
    if (p != nullptr || *moved != 7) { std::printf("FAIL std::move\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_11_94_utility_move\n");
    return failures;
}
