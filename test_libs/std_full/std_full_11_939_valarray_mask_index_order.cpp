#include <string>
#include <valarray>
#include <cstdio>
int main()
{
    int failures = 0;
    std::valarray<int> big(8);
    for (int i = 0; i < 8; i++) big[i] = i;
    int scalar_sum = 0;
    for (int i = 0; i < 8; i++) scalar_sum += big[i];
    std::valarray<bool> mask = big > 4;
    std::valarray<int> ms = big[mask];
    if (scalar_sum != 28 || ms.size() != 3 || ms[0] != 5 || ms[2] != 7) { printf("FAIL mask after scalar\n"); failures |= 1; }
    std::valarray<size_t> idx(3);
    idx[0] = 7; idx[1] = 0; idx[2] = 2;
    std::valarray<int> ind = big[idx];
    if (ind.size() != 3 || ind[0] != 7 || ind[1] != 0 || ind[2] != 2) { printf("FAIL indirect after scalar\n"); failures |= 2; }
    if (failures == 0) printf("PASS std_full_11_939_valarray_mask_index_order\n");
    return failures;
}
