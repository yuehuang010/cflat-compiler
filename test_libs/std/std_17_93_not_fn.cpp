// C++20 equivalent of std_17_93_not_fn.cb (compile-time parity baseline)
#include <string>
#include <functional>
#include <cstdio>
bool is_zero(int x) { return x == 0; }
int main() {
    int failures = 0;
    auto pred = std::not_fn(is_zero);
    if (!pred(1) || pred(0)) { std::printf("FAIL not_fn\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_17_93_not_fn\n");
    return failures;
}
