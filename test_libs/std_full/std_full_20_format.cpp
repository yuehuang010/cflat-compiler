#include <string>
#include <cstdio>
#include <format>
int main()
{
    int failures = 0;
    if (std::format("{:*<6}", "x") != "x*****") failures |= 1;
    if (std::format("{:+d} {:#x}", 42, 42) != "+42 0x2a") failures |= 2;
    if (std::format("{:.3f} {:.1e}", 1.25, 12.0) != "1.250 1.2e+01") failures |= 4;
    if (std::format("{1}:{0}", "left", "right") != "right:left") failures |= 8;
    if (std::format("{} {}", true, 'Q') != "true Q") failures |= 16;
    char output[16] = {};
    auto end = std::format_to(output, "{:04d}", 7);
    std::string written(output, end);
    if (written != "0007" || std::formatted_size("{}!", "ok") != 3) failures |= 32;
    std::string fmt = "fmt";
    int nine = 9;
    auto args = std::make_format_args(fmt, nine);
    if (std::vformat("{}:{}", args) != "fmt:9") failures |= 64;
    char vbuf[16] = {};
    int eleven = 11;
    auto eleven_args = std::make_format_args(eleven);
    auto vend = std::vformat_to(vbuf, "{}", eleven_args);
    if (std::string(vbuf, vend) != "11") failures |= 128;
    if (failures & 1) std::puts("FAIL fill/align");
    if (failures & 2) std::puts("FAIL sign/alternate hex");
    if (failures & 4) std::puts("FAIL float precision");
    if (failures & 8) std::puts("FAIL positional arguments");
    if (failures & 16) std::puts("FAIL bool/char format");
    if (failures & 32) std::puts("FAIL format_to/formatted_size");
    if (failures & 64) std::puts("FAIL vformat");
    if (failures & 128) std::puts("FAIL vformat_to");
    if (failures == 0) std::puts("PASS std_full_20_format");
    return failures;
}
