#include <string>
#include <string_view>
#include <functional>
#include <cstdio>
int main()
{
    int failures = 0;
    std::string a("abcdef", 3);
    std::string b(4, 'x');
    std::string c("hello world", 6, 5);
    std::string d(a.begin(), a.end());
    if (a != "abc" || b != "xxxx" || c != "world" || d != "abc") { printf("FAIL ctors\n"); failures |= 1; }
    std::string s = "hello world";
    s.insert(5, ",");
    s.insert(0, 2, '>');
    s.erase(0, 2);
    s.replace(0, 5, "HELLO");
    if (s != "HELLO, world") { printf("FAIL insert/erase/replace\n"); failures |= 2; }
    std::string r = "abcabcabc";
    if (r.rfind("abc") != 6 || r.rfind('b', 5) != 4 || r.find_first_of("cb") != 1 || r.find_last_of("a") != 6 || r.find_first_not_of("ab") != 2 || r.find_last_not_of("c") != 7 || r.find("zzz") != std::string::npos)
    { printf("FAIL find family\n"); failures |= 4; }
    std::string cap;
    cap.reserve(100);
    bool capok = cap.capacity() >= 100 && cap.size() == 0 && cap.empty();
    cap = "abc"; cap.shrink_to_fit();
    if (!capok || cap.length() != 3 || cap.capacity() < 3) { printf("FAIL capacity\n"); failures |= 8; }
    size_t idx = 0;
    int v1 = std::stoi("  42abc", &idx);
    size_t idx2 = 0;
    long v2 = std::stol("ff", &idx2, 16);
    int v3 = std::stoi("1010", nullptr, 2);
    unsigned long v4 = std::stoul("777", nullptr, 8);
    long long v5 = std::stoll("-9000000000");
    float v6 = std::stof("1.5");
    if (v1 != 42 || idx != 4 || v2 != 255 || idx2 != 2 || v3 != 10 || v4 != 511 || v5 != -9000000000LL || v6 != 1.5f)
    { printf("FAIL sto*\n"); failures |= 16; }
    std::wstring w = std::to_wstring(123);
    w += L"45";
    std::wstring w2(L"abc");
    w2.append(2, L'z');
    if (w != L"12345" || w.size() != 5 || w2 != L"abczz" || w2.find(L'z') != 3) { printf("FAIL wstring\n"); failures |= 32; }
    std::u8string ustr(3, (char8_t)'a');
    ustr.append(2, (char8_t)'d');
    if (ustr.size() != 5 || ustr[0] != 'a') { printf("FAIL u8string\n"); failures |= 64; }
    std::string_view sv = "hello world";
    sv.remove_suffix(6);
    char buf[8] = {0};
    size_t n = std::string_view("abcdef").copy(buf, 3, 2);
    if (sv != "hello" || n != 3 || std::string(buf) != "cde" || std::string_view("abcabc").rfind("bc") != 4)
    { printf("FAIL string_view\n"); failures |= 128; }
    if (std::hash<std::string_view>()("same text") != std::hash<std::string>()("same text")) { printf("FAIL hash\n"); failures |= 256; }
    if (failures == 0) printf("PASS std_full_11_strings\n");
    return failures;
}
