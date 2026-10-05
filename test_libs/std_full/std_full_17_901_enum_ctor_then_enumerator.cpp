// C++20 equivalent of std_full_17_901_enum_ctor_then_enumerator.cb
#include <string>
#include <system_error>
#include <cstdio>

int main() {
    int failures = 0;
    std::errc zero = std::errc();
    std::errc inv = std::errc::invalid_argument;
    if (zero == inv || zero != std::errc()) { std::printf("FAIL enum_ctor_then_enumerator\n"); failures |= 1; }
    if (failures == 0) std::printf("PASS std_full_17_901_enum_ctor_then_enumerator\n");
    return failures;
}
