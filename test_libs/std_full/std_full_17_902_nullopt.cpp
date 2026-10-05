// C++20 equivalent of std_full_17_902_nullopt.cb
#include <string>
#include <optional>
#include <cstdio>

int main() {
    int failures = 0;
    std::optional<int> a = std::make_optional(5);
    std::optional<int> b;
    std::optional<int> c = std::nullopt;
    if (b != std::nullopt || a == std::nullopt || !(c == std::nullopt)) { std::printf("FAIL nullopt_compare\n"); failures |= 1; }
    a = std::nullopt;
    if (a.has_value()) { std::printf("FAIL nullopt_assign\n"); failures |= 2; }
    if (failures == 0) std::printf("PASS std_full_17_902_nullopt\n");
    return failures;
}
