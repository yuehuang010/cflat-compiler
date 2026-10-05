#include <string>
#include <cstddef>
#include <cfloat>
#include <climits>
#include <cstdint>
#include <cinttypes>
#include <cmath>
#include <limits>
#include <cstdio>
int main()
{
    int failures = 0;
    if (CHAR_BIT != 8 || INT_MAX != 2147483647 || INT_MIN != -2147483647 - 1 || UCHAR_MAX != 255 || SHRT_MAX != 32767 || LLONG_MAX != 9223372036854775807LL || LLONG_MIN != -9223372036854775807LL - 1 || UINT_MAX != 4294967295u)
    { printf("FAIL climits\n"); failures |= 16; }
    if (INT64_MAX != 9223372036854775807LL || INT8_MIN != -128 || UINT16_MAX != 65535 || INT32_MAX != 2147483647 || UINT8_MAX != 255)
    { printf("FAIL cstdint limits\n"); failures |= 32; }
    int64_t i64v = INT64_MAX; uint8_t u8v = 200; int16_t i16v = -5;
    if (i64v != std::numeric_limits<long long>::max() || u8v != 200 || i16v != -5) { printf("FAIL cstdint types\n"); failures |= 64; }
    if (std::numeric_limits<int>::lowest() != INT_MIN || std::numeric_limits<int>::digits != 31 || !std::numeric_limits<int>::is_signed || std::numeric_limits<unsigned>::is_signed || std::numeric_limits<unsigned char>::digits != 8 || std::numeric_limits<double>::digits != 53 || !std::numeric_limits<short>::is_integer || std::numeric_limits<double>::is_integer || std::numeric_limits<double>::radix != 2)
    { printf("FAIL limits ints\n"); failures |= 512; }
    if (failures == 0) printf("PASS std_full_11_944_climits_cstdint_macros\n");
    return failures;
}
