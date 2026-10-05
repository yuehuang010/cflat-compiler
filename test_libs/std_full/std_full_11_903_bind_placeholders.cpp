// C++20 equivalent of std_full_11_903_bind_placeholders.cb (compile-time parity baseline)
#include <string>
#include <functional>
#include <cstdio>

int add3(int a, int b, int c) { return a * 100 + b * 10 + c; }

int main() {
    int failures = 0;
    auto b1 = std::bind(add3, 1, std::placeholders::_1, 3);
    auto b2 = std::bind(add3, std::placeholders::_2, std::placeholders::_1, 0);
    auto b3 = std::bind(std::plus<int>(), std::placeholders::_1, 100);
    if (b1(2) != 123 || b2(1, 2) != 210 || b3(5) != 105) { printf("FAIL bind\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_903_bind_placeholders\n");
    return failures;
}
