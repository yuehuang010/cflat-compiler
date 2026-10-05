// C++20 equivalent of std_full_11_908_cwrapper_macros.cb (compile-time parity baseline)
#include <string>
#include <climits>
#include <cfloat>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

int main() {
    int failures = 0;
    if (CHAR_BIT != 8 || INT_MAX != 2147483647) { printf("FAIL climits\n"); failures |= 1; }
    if (DBL_DIG != 15 || FLT_RADIX != 2) { printf("FAIL cfloat\n"); failures |= 2; }
    if (INT64_MAX != 9223372036854775807LL || UINT8_MAX != 255) { printf("FAIL cstdint\n"); failures |= 4; }
    if (RAND_MAX < 32767 || EXIT_FAILURE == 0) { printf("FAIL cstdlib\n"); failures |= 8; }
    if (failures == 0) printf("PASS std_full_11_908_cwrapper_macros\n");
    return failures;
}
