// C++20 equivalent of std_full_11_cstdlib.cb (compile-time parity baseline)
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstring>

int cmp_int(const void* a, const void* b) { int x = *(const int*)a; int y = *(const int*)b; return (x > y) - (x < y); }

int main() {
    int failures = 0;
    if (std::abs(-5) != 5 || std::labs(-6L) != 6 || std::llabs(-7LL) != 7 || std::abs(-2.5) != 2.5) { printf("FAIL abs\n"); failures |= 1; }
    char* end = nullptr; char** endp = &end;
    long a = std::strtol("123abc", endp, 10);
    if (a != 123 || std::strcmp(end, "abc") != 0) { printf("FAIL strtol endptr\n"); failures |= 2; }
    long hex = std::strtol("ff", nullptr, 16);
    long long ll = std::strtoll("-9000000000", nullptr, 10);
    unsigned long ul = std::strtoul("77", nullptr, 8);
    if (hex != 255 || ll != -9000000000LL || ul != 63) { printf("FAIL strto base\n"); failures |= 4; }
    double d = std::strtod("2.5rest", endp);
    if (d != 2.5 || std::strcmp(end, "rest") != 0 || std::atoi("42") != 42 || std::atol("-8") != -8 || std::atof("0.5") != 0.5)
    { printf("FAIL strtod atoi\n"); failures |= 8; }
    int arr[5] = {5, 3, 9, 1, 7};
    std::qsort(arr, 5, sizeof(int), cmp_int);
    if (arr[0] != 1 || arr[1] != 3 || arr[2] != 5 || arr[3] != 7 || arr[4] != 9) { printf("FAIL qsort\n"); failures |= 16; }
    int key = 7; int missing = 4;
    int* found = (int*)std::bsearch(&key, arr, 5, sizeof(int), cmp_int);
    int* absent = (int*)std::bsearch(&missing, arr, 5, sizeof(int), cmp_int);
    if (found == nullptr || *found != 7 || found != arr + 3 || absent != nullptr) { printf("FAIL bsearch\n"); failures |= 32; }
    int* m = (int*)std::malloc(4 * sizeof(int));
    for (int i = 0; i < 4; i++) m[i] = i * 10;
    m = (int*)std::realloc(m, 8 * sizeof(int));
    for (int i = 4; i < 8; i++) m[i] = i * 10;
    bool mem_ok = m[3] == 30 && m[7] == 70;
    std::free(m);
    int* z = (int*)std::calloc(6, sizeof(int));
    bool zero_ok = z[0] == 0 && z[5] == 0;
    std::free(z);
    if (!mem_ok || !zero_ok) { printf("FAIL malloc family\n"); failures |= 64; }
    if (std::getenv("PATH") == nullptr || std::getenv("CFLAT_NO_SUCH_VARIABLE_XYZ") != nullptr) { printf("FAIL getenv\n"); failures |= 128; }
    std::srand(42);
    bool rand_ok = true;
    for (int i = 0; i < 20; i++) { int r = std::rand(); if (r < 0 || r > RAND_MAX) rand_ok = false; }
    if (!rand_ok || RAND_MAX < 32767) { printf("FAIL rand range\n"); failures |= 256; }
    if (EXIT_SUCCESS != 0 || EXIT_FAILURE == 0) { printf("FAIL exit codes\n"); failures |= 512; }
    if (failures == 0) printf("PASS std_full_11_cstdlib\n");
    return failures;
}
