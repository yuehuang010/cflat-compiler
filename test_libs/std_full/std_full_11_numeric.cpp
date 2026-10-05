// C++20 equivalent of std_full_11_numeric.cb
#include <string>
#include <functional>
#include <numeric>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::vector<int> v{1, 2, 3, 4};
    std::vector<int> ps(4, 0);
    std::partial_sum(v.begin(), v.end(), ps.begin());
    std::vector<int> pm(4, 0);
    std::partial_sum(v.begin(), v.end(), pm.begin(), std::multiplies<int>());
    std::vector<int> ad(4, 0);
    std::adjacent_difference(v.begin(), v.end(), ad.begin());
    if (ps[0] != 1 || ps[1] != 3 || ps[3] != 10 || pm[3] != 24 || pm[2] != 6 || ad[0] != 1 || ad[1] != 1 || ad[3] != 1) { std::printf("FAIL partial_sum_adjacent_difference\n"); failures |= 1; }
    std::vector<int> ic(4, 0), ec(4, 0);
    std::inclusive_scan(v.begin(), v.end(), ic.begin());
    std::exclusive_scan(v.begin(), v.end(), ec.begin(), 100);
    if (ic[3] != 10 || ic[0] != 1 || ec[0] != 100 || ec[1] != 101 || ec[3] != 106) { std::printf("FAIL scans\n"); failures |= 2; }
    std::vector<int> tic(4, 0), tec(4, 0);
    std::transform_inclusive_scan(v.begin(), v.end(), tic.begin(), std::plus<int>(), [](int x) { return x * 2; });
    std::transform_exclusive_scan(v.begin(), v.end(), tec.begin(), 0, std::plus<int>(), [](int x) { return x * 2; });
    if (tic[0] != 2 || tic[3] != 20 || tec[0] != 0 || tec[1] != 2 || tec[3] != 12) { std::printf("FAIL transform_scans\n"); failures |= 4; }
    int red = std::reduce(v.begin(), v.end());
    int red_init = std::reduce(v.begin(), v.end(), 10);
    int red_op = std::reduce(v.begin(), v.end(), 1, std::multiplies<int>());
    int tr = std::transform_reduce(v.begin(), v.end(), v.begin(), 0);
    int tr2 = std::transform_reduce(v.begin(), v.end(), 0, std::plus<int>(), [](int x) { return x * x; });
    if (red != 10 || red_init != 20 || red_op != 24 || tr != 30 || tr2 != 30) { std::printf("FAIL reduce\n"); failures |= 8; }
    int acc = std::accumulate(v.begin(), v.end(), 0);
    int acc_op = std::accumulate(v.begin(), v.end(), 1, std::multiplies<int>());
    int ip = std::inner_product(v.begin(), v.end(), v.begin(), 0);
    int ip2 = std::inner_product(v.begin(), v.end(), v.begin(), 0, std::plus<int>(), std::multiplies<int>());
    std::vector<int> io(4, 0);
    std::iota(io.begin(), io.end(), 5);
    if (acc != 10 || acc_op != 24 || ip != 30 || ip2 != 30 || io[0] != 5 || io[3] != 8) { std::printf("FAIL accumulate_iota\n"); failures |= 16; }
    if (std::gcd(12, 18) != 6 || std::lcm(4, 6) != 12 || std::gcd(0, 5) != 5 || std::midpoint(10, 20) != 15 || std::midpoint(1, 4) != 2) { std::printf("FAIL gcd_lcm_midpoint\n"); failures |= 32; }
    if (failures == 0) std::printf("PASS std_full_11_numeric\n");
    return failures;
}
