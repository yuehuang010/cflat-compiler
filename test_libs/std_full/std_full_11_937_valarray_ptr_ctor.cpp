#include <valarray>
#include <cstdio>
int main()
{
    int failures = 0;
    int arr[4] = {4, 1, 3, 2};
    int* p = &arr[0];
    std::valarray<int> w(p, 4);
    if (w[0] != 4 || w[1] != 1 || w[2] != 3 || w[3] != 2) { printf("FAIL valarray ptr ctor\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_937_valarray_ptr_ctor\n");
    return failures;
}
