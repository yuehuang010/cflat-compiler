#include <string>
#include <bitset>
#include <functional>
#include <cstdio>
int main()
{
    int failures = 0;
    std::bitset<8> d;
    d[2] = true; d[4] = 1; d[2].flip(); d[6] = d[4];
    bool bit2 = d[2], bit4 = d[4], bit6 = d[6];
    bool inv = ~d[0];
    if (bit2 || !bit4 || !bit6 || !inv || d.count() != 2 || d.to_ullong() != 0x50) { printf("FAIL proxy\n"); failures |= 16; }
    if (failures == 0) printf("PASS std_full_11_941_bitset_proxy\n");
    return failures;
}
