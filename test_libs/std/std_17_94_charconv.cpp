// C++20 equivalent of std_17_94_charconv.cb (compile-time parity baseline)
#include <string>
#include <charconv>
#include <cstdio>
int main() {
    int failures = 0;
    char buffer[16]{};
    auto converted = std::to_chars(buffer, buffer + 16, 1234);
    int parsed = 0;
    auto result = std::from_chars(buffer, converted.ptr, parsed);
    if (converted.ec != std::errc{} || result.ec != std::errc{} || parsed != 1234) { std::printf("FAIL charconv\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_17_94_charconv\n");
    return failures;
}
