// C++20 equivalent of std_11_strings.cb (compile-time parity baseline)
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdio>

int main() {
    int failures = 0;
    std::string s("hello"); s.append(" world");
    if (s.find("world") != 6 || s.substr(0, 5) != "hello" || s.compare("hello world") != 0 || std::strcmp(s.c_str(), "hello world") != 0) { std::printf("FAIL string\n"); failures |= 1; }
    if (std::to_string(42) != "42" || std::stoi("-12") != -12 || std::stol("123456") != 123456L || std::stod("2.5") != 2.5 || std::u16string(u"abc").size() != 3 || std::u32string(U"xy").size() != 2) { std::printf("FAIL conversions\n"); failures |= 2; }
    if (std::tolower('A') != 'a' || std::isalpha('z') == 0 || std::strlen("abc") != 3 || sizeof(std::int32_t) != 4) { std::printf("FAIL c headers\n"); failures |= 4; }
    if (!failures) std::printf("PASS std_11_strings\n");
    return failures;
}
