// C++20 equivalent of std_11_93_tuple_tie.cb (compile-time parity baseline)
#include <tuple>
#include <cstdio>
#include <string>

int main() {
    int failures = 0;
    int x = 0, y = 0;
    auto values = std::make_tuple(4, 5);
    std::tie(x, y) = values;
    if (x != 4 || y != 5) { std::printf("FAIL tuple tie\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_11_93_tuple_tie\n");
    return failures;
}
