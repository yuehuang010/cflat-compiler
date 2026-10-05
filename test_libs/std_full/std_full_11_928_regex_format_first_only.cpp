#include <string>
#include <cstdio>
#include <regex>
int main()
{
    std::regex digits("([0-9]+)");
    std::string replaced = std::regex_replace(std::string("ab12cd34"), digits, "<$1>", std::regex_constants::format_first_only);
    if (replaced != "ab<12>cd34") { std::puts("FAIL format_first_only"); return 1; }
    std::puts("PASS std_full_11_928_regex_format_first_only"); return 0;
}
