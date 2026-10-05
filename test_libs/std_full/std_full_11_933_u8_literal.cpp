#include <string>
#include <cstdio>
int main()
{
    int failures = 0;
    std::u8string a = u8"abc";
    std::u8string b(u8"xyz");
    if (a.size() != 3 || b.size() != 3) { printf("FAIL u8 literal ctor\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_933_u8_literal\n");
    return failures;
}
