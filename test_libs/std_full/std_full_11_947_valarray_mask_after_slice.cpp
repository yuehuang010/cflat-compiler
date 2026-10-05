#include <string>
#include <complex>
#include <valarray>
#include <cstdio>
int main()
{
    int failures = 0;
    std::valarray<int> big(8);
    for (int i = 0; i < 8; i++) big[i] = i;
    std::valarray<int> sl = big[std::slice(1, 3, 2)];
    if (sl.size() != 3 || sl[0] != 1 || sl[1] != 3 || sl[2] != 5) { printf("FAIL slice\n"); failures |= 128; }
    std::valarray<bool> mask = big > 4;
    std::valarray<int> ms = big[mask];
    if (ms.size() != 3 || ms[0] != 5 || ms[2] != 7) { printf("FAIL mask\n"); failures |= 512; }
    if (failures == 0) printf("PASS std_full_11_947_valarray_mask_after_slice\n");
    return failures;
}
