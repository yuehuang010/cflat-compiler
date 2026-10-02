// C++20 equivalent of std_20_95_format_to_n.cb (compile-time parity baseline)
#include <format>
#include <string>

int main()
{
    int failures = 0;
    char output[16] = {};
    auto written = std::format_to_n(output, sizeof(output) - 1, "value={}", 42);
    output[written.size] = '\0';
    if (std::string(output) != "value=42" || written.size != 8) failures |= 1;
    return failures;
}
