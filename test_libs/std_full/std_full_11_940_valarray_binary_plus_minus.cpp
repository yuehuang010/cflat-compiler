#include <string>
#include <valarray>
#include <cstdio>
int main()
{
    int failures = 0;
    int arr[4] = {4, 1, 3, 2};
    std::valarray<int> w(arr, 4);
    std::valarray<int> x = w * 2;
    std::valarray<int> y = w + 1;
    std::valarray<int> z = w + w;
    if (x[0] != 8 || x[3] != 4 || y[1] != 2 || z[2] != 6 || x.sum() != 20) { printf("FAIL valarray ops\n"); failures |= 16; }
    if (failures == 0) printf("PASS std_full_11_940_valarray_binary_plus_minus\n");
    return failures;
}
