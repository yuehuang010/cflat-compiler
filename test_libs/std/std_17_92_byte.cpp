// C++20 equivalent of std_17_92_byte.cb (compile-time parity baseline)
#include <string>
#include <cstddef>
#include <cstdio>
int main() {
    int failures = 0;
    std::byte value{42};
    int result = std::to_integer<int>(value);
    if (result != 42) { std::printf("FAIL byte: got %d want 42\n", result); failures |= 1; }
    if (!failures) std::printf("PASS std_17_92_byte\n");
    return failures;
}
