// C++20 equivalent of std_full_11_927_qsort_comparator.cb (compile-time parity baseline)
#include <string>
#include <cstdlib>
#include <cstdio>

int cmp_int(const void* a, const void* b) { int x = *(const int*)a; int y = *(const int*)b; return (x > y) - (x < y); }

int main() {
    int failures = 0;
    int arr[4] = {4, 2, 8, 6};
    std::qsort(arr, 4, sizeof(int), cmp_int);
    if (arr[0] != 2 || arr[1] != 4 || arr[2] != 6 || arr[3] != 8) { printf("FAIL std qsort\n"); failures |= 1; }
    int key = 6;
    int* found = (int*)std::bsearch(&key, arr, 4, sizeof(int), cmp_int);
    if (found == nullptr || *found != 6) { printf("FAIL std bsearch\n"); failures |= 2; }
    if (failures == 0) printf("PASS std_full_11_927_qsort_comparator\n");
    return failures;
}
