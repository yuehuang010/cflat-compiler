#include <cwchar>
#include <cstdio>
int main()
{
    int failures = 0;
    wchar_t w[16] = {0};
    std::wcscpy(w, L"wide");
    if (std::wcschr(w, L'd') != w + 2) { printf("FAIL wcschr\n"); failures |= 1; }
    if (std::wcsstr(w, L"id") != w + 1) { printf("FAIL wcsstr\n"); failures |= 2; }
    if (failures == 0) printf("PASS std_full_11_938_wcs_search_overloads\n");
    return failures;
}
