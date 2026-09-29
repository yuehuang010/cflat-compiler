// C++20 equivalent of fmt_01_runtime.cb (compile-time parity baseline)
#include <cstdio>
#include <string>
#include <fmt/format.h>

int main()
{
    int failures = 0;
    std::string s = fmt::format(fmt::runtime("{}-{}"), 7, 3.5);
    if (s != "7-3.5") { std::printf("FAIL format: got length %d want 5\n", (int)s.size()); failures |= 1; }
    std::string converted = fmt::to_string(42);
    if (converted.size() != 2) { std::printf("FAIL to_string: got %d want 2\n", (int)converted.size()); failures |= 2; }
    std::string printed = fmt::format(fmt::runtime("v={}\n"), 5);
    if (printed != "v=5\n") { std::printf("FAIL print: got length %d want 4\n", (int)printed.size()); failures |= 4; }
    fmt::print(fmt::runtime("v={}\n"), 5);
    if (failures == 0) std::printf("PASS fmt_01_runtime\n");
    return failures;
}
