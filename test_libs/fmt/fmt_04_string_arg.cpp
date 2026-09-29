// C++20 equivalent of fmt_04_string_arg.cb (compile-time parity baseline)
#include <cstdio>
#include <string>
#include <fmt/format.h>

int main()
{
    int failures = 0;
    std::string name = "world";
    std::string s = fmt::format(fmt::runtime("hello {}"), name);
    if (s.size() != 11) { std::printf("FAIL size: got %d want 11\n", (int)s.size()); failures |= 1; }
    if (failures == 0) std::printf("PASS fmt_04_string_arg\n");
    return failures;
}
