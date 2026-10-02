// C++20 equivalent of std_14_91_make_unique_array.cb (compile-time parity baseline)
#include <string>
#include <memory>
#include <cstdio>
int main() {
    int failures = 0;
    auto values = std::make_unique<int[]>(2);
    values[0] = 4; values[1] = 5;
    if (values[0] != 4 || values[1] != 5) { std::printf("FAIL make_unique_array\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_14_91_make_unique_array\n");
    return failures;
}
