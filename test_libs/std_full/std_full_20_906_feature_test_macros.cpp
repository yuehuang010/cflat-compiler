#include <string>
#include <cstdio>
#include <version>
int main()
{
    int failures = 0;
#if !defined(__cpp_lib_coroutine) || __cpp_lib_coroutine <= 0
    printf("FAIL coroutine macro\n"); failures |= 1;
#endif
#if !defined(__cpp_lib_source_location) || __cpp_lib_source_location <= 0
    printf("FAIL source_location macro\n"); failures |= 2;
#endif
#if !defined(__cpp_lib_ranges) || __cpp_lib_ranges <= 0
    printf("FAIL ranges macro\n"); failures |= 4;
#endif
#if !defined(__cpp_lib_format) || __cpp_lib_format <= 0
    printf("FAIL format macro\n"); failures |= 8;
#endif
    if (failures == 0) printf("PASS std_full_20_906_feature_test_macros\n");
    return failures;
}
