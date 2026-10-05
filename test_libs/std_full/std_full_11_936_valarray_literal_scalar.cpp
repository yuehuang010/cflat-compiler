#include <valarray>
#include <cstdio>
int main()
{
    int failures = 0;
    int arr[4] = {4, 1, 3, 2};
    std::valarray<int> w(arr, 4);
    std::valarray<int> x = w + 100;
    std::valarray<int> y = w * 3;
    if (x[0] != 104 || x[1] != 101 || x[3] != 102 || y[0] != 12 || y[1] != 3 || y[2] != 9) { printf("FAIL valarray literal scalar\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_936_valarray_literal_scalar\n");
    return failures;
}
