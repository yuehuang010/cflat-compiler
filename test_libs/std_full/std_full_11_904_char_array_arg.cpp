#include <string>
#include <cstring>
#include <cstdio>
int main()
{
    int failures = 0;
    char buf[8] = {0};
    buf[0] = 'a'; buf[1] = 'b';
    if (std::strlen(buf) != 2) { printf("FAIL strlen(array)\n"); failures |= 1; }
    if (std::string(buf) != "ab") { printf("FAIL string(array)\n"); failures |= 2; }
    if (std::strcmp(buf, "ab") != 0) { printf("FAIL strcmp(array)\n"); failures |= 4; }
    if (failures == 0) printf("PASS std_full_11_904_char_array_arg\n");
    return failures;
}
