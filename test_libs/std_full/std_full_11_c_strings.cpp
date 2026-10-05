#include <string>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
int main()
{
    int failures = 0;
    char d[32] = {0};
    std::strcpy(d, "foo");
    std::strcat(d, "bar");
    char d2[8] = {0};
    std::strncpy(d2, "abcdef", 3);
    const char* hay = "hello world hello";
    if (std::strlen(d) != 6 || std::strcmp(d, "foobar") != 0 || std::strncmp(d, "fooxyz", 3) != 0 || std::strcmp(d2, "abc") != 0
        || std::strchr(hay, 'w') != hay + 6 || std::strrchr(hay, 'h') != hay + 12 || std::strstr(hay, "world") != hay + 6 || std::strchr(hay, 'z') != nullptr)
    { printf("FAIL str*\n"); failures |= 1; }
    char tokbuf[] = "a,b;c";
    int ntok = 0;
    char* t = std::strtok(tokbuf, ",;");
    std::string joined;
    while (t != nullptr) { ntok++; joined += t; t = std::strtok(nullptr, ",;"); }
    if (ntok != 3 || joined != "abc") { printf("FAIL strtok\n"); failures |= 2; }
    char m1[8]; char m2[8];
    std::memset(m1, 'q', 8);
    std::memcpy(m2, m1, 8);
    std::memmove(m2 + 1, m2, 4);
    m2[0] = 'a';
    if (std::memcmp(m1, m2, 8) == 0 || std::memcmp(m1, m1, 8) != 0 || std::memchr(m2, 'a', 8) != m2 || std::memchr(m1, 'z', 8) != nullptr || m2[1] != 'q')
    { printf("FAIL mem*\n"); failures |= 4; }
    if (std::strerror(1) == nullptr) { printf("FAIL strerror\n"); failures |= 8; }
    if (!std::isalpha('a') || std::isalpha('1') || !std::isdigit('7') || std::isdigit('x') || !std::isalnum('z') || !std::isspace(' ') || std::isspace('x')
        || !std::ispunct('!') || std::ispunct('a') || !std::isxdigit('f') || std::isxdigit('g') || !std::isupper('A') || std::isupper('a') || !std::islower('a')
        || !std::isprint('a') || std::isprint('\n') || !std::iscntrl('\n') || !std::isgraph('a') || std::isgraph(' ') || !std::isblank('\t') || std::isblank('x')
        || std::tolower('Q') != 'q' || std::toupper('q') != 'Q' || std::toupper('1') != '1')
    { printf("FAIL cctype\n"); failures |= 16; }
    if (!std::iswalpha(L'a') || std::iswalpha(L'1') || !std::iswdigit(L'5') || !std::iswspace(L' ') || std::towupper(L'a') != L'A' || std::towlower(L'Z') != L'z'
        || !std::iswctype(L'a', std::wctype("alpha")) || std::iswctype(L'1', std::wctype("alpha")))
    { printf("FAIL cwctype\n"); failures |= 32; }
    wchar_t w1[16] = {0};
    std::wcscpy(w1, L"wide");
    wchar_t* endp = nullptr;
    long wl = std::wcstol(L"123xyz", &endp, 10);
    double wd = std::wcstod(L"2.5", nullptr);
    wchar_t wbuf[8];
    std::wmemset(wbuf, L'k', 8);
    if (std::wcslen(w1) != 4 || std::wcscmp(w1, L"wide") != 0 || std::wcscmp(L"a", L"b") >= 0 || wl != 123 || *endp != L'x' || wd != 2.5 || wbuf[7] != L'k')
    { printf("FAIL wcs*\n"); failures |= 64; }
    wchar_t wout[16] = {0};
    size_t mb = std::mbstowcs(wout, "abc", 16);
    char nout[16] = {0};
    size_t wb = std::wcstombs(nout, L"xyz", 16);
    wchar_t sw[16] = {0};
    int swn = std::swprintf(sw, 16, L"n=%d", 42);
    if (mb != 3 || std::wcscmp(wout, L"abc") != 0 || wb != 3 || std::strcmp(nout, "xyz") != 0 || swn != 4 || std::wcscmp(sw, L"n=42") != 0)
    { printf("FAIL conversions\n"); failures |= 128; }
    if (failures == 0) printf("PASS std_full_11_c_strings\n");
    return failures;
}
