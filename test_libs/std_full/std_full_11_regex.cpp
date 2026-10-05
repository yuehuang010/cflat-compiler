#include <string>
#include <cstdio>
#include <regex>
#include <vector>
int main()
{
    int failures = 0;
    std::string text = "xx12yy345zz";
    std::regex digits("([0-9]+)");
    std::smatch match;
    if (!std::regex_search(text, match, digits) || match.size() != 2 || match.position(1) != 2 || match.str(1) != "12" || match.prefix().str() != "xx" || match.suffix().str() != "yy345zz") failures |= 1;
    std::regex word("[a-z]+");
    int count = 0;
    for (std::sregex_iterator i(text.begin(), text.end(), word), end; i != end; ++i) ++count;
    if (count != 3) failures |= 2;
    std::string csv = "one,two,three";
    std::regex comma(",");
    std::vector<std::string> fields;
    for (std::sregex_token_iterator i(csv.begin(), csv.end(), comma, -1), end; i != end; ++i) fields.push_back(*i);
    if (fields.size() != 3 || fields[0] != "one" || fields[1] != "two" || fields[2] != "three") failures |= 4;
    std::regex lower("abc", std::regex::icase);
    if (!std::regex_match(std::string("AbC"), lower)) failures |= 8;
    if (failures & 1) std::puts("FAIL match results context");
    if (failures & 2) std::puts("FAIL regex iterator count");
    if (failures & 4) std::puts("FAIL token iterator split");
    if (failures & 8) std::puts("FAIL regex icase");
    if (failures == 0) std::puts("PASS std_full_11_regex");
    return failures;
}
