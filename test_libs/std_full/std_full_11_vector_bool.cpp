// C++20 equivalent of std_full_11_vector_bool.cb
#include <string>
#include <algorithm>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::vector<bool> vb(5, false);
    vb[1] = true; vb[3] = true;
    if (vb.size() != 5 || vb[0] || !vb[1] || vb[2] || !vb[3]) { std::printf("FAIL vb_read_write\n"); failures |= 1; }
    vb.flip();
    if (!vb[0] || vb[1] || !vb[2] || vb[3] || !vb[4]) { std::printf("FAIL vb_flip\n"); failures |= 2; }
    vb[0].flip();
    if (vb[0]) { std::printf("FAIL vb_ref_flip\n"); failures |= 4; }
    std::vector<bool>::reference r0 = vb[0];
    r0 = true;
    bool copied = r0;
    if (!vb[0] || !copied) { std::printf("FAIL vb_proxy_ref\n"); failures |= 8; }
    std::vector<bool>::swap(vb[0], vb[1]);
    if (vb[0] || !vb[1]) { std::printf("FAIL vb_swap_refs\n"); failures |= 16; }
    // vb = F T T F T
    long cnt = std::count(vb.begin(), vb.end(), true);
    if (cnt != 3) { std::printf("FAIL vb_count\n"); failures |= 32; }
    vb.push_back(true); vb.pop_back(); vb.resize(7, true);
    if (vb.size() != 7 || !vb[6] || vb.back() != true) { std::printf("FAIL vb_resize\n"); failures |= 64; }
    std::vector<bool> other(2, true);
    vb.swap(other);
    if (vb.size() != 2 || other.size() != 7) { std::printf("FAIL vb_swap\n"); failures |= 128; }
    if (failures == 0) std::printf("PASS std_full_11_vector_bool\n");
    return failures;
}
