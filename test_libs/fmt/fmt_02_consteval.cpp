// C++20 equivalent of fmt_02_consteval.cb (compile-time parity baseline)
#include <cstdio>
#include <string>
#include <fmt/format.h>

int main()
{
    int failures = 0;
    std::string s = fmt::format("{}-{}", 7, 3.5);
    if (s != "7-3.5" || s.size() != 5) { std::printf("FAIL format: got %d want 5\n", (int)s.size()); failures |= 1; }
    fmt::print("print={}\n", 7);
    if (failures == 0) std::printf("PASS fmt_02_consteval\n");
    return failures;
}
