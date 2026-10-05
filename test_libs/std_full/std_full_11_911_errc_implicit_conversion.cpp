// C++20 equivalent of std_full_11_911_errc_implicit_conversion.cb (compile-time parity baseline)
#include <string>
#include <system_error>
#include <cerrno>
#include <cstdio>

int main() {
    int failures = 0;
    std::error_code mk = std::make_error_code(std::errc::invalid_argument);
    if (!(mk == std::errc::invalid_argument) || mk == std::errc::no_such_file_or_directory)
    { printf("FAIL error_code == errc\n"); failures |= 1; }
    std::error_condition cond = std::errc::invalid_argument;
    if (cond.value() != EINVAL || !(cond == std::errc::invalid_argument)) { printf("FAIL error_condition from errc\n"); failures |= 2; }
    if (failures == 0) printf("PASS std_full_11_911_errc_implicit_conversion\n");
    return failures;
}
