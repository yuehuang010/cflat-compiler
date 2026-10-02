// C++20 equivalent of std_11_errors_regex.cb (compile-time parity baseline)
#include <regex>
#include <string>
#include <system_error>
#include <cstdio>

int main() {
    int failures = 0;
    std::error_code ec = std::make_error_code(std::errc::invalid_argument);
    std::string text("ab12cd34"); std::regex digits("[0-9]+");
    if (ec.value() != 22) { std::printf("FAIL error_code value: got %d want 22\n", ec.value()); failures |= 1; }
    if (!std::regex_search(text, digits) || !std::regex_match(std::string("123"), digits) || std::regex_replace(text, digits, "#") != "ab#cd#") { std::printf("FAIL regex operations\n"); failures |= 2; }
    if (!failures) std::printf("PASS std_11_errors_regex\n");
    return failures;
}
