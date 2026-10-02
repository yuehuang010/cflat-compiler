// C++20 equivalent of std_11_92_function_lambda.cb (compile-time parity baseline)
#include <functional>
#include <cstdio>
#include <string>

int main() {
    int failures = 0;
    std::function<int(int)> f = [](int x) { return x + 3; };
    if (f(4) != 7) { std::printf("FAIL std::function lambda\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_11_92_function_lambda\n");
    return failures;
}
