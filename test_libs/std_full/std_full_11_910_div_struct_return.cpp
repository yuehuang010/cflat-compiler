// C++20 equivalent of std_full_11_910_div_struct_return.cb (compile-time parity baseline)
#include <string>
#include <cstdlib>
#include <cinttypes>
#include <cstdio>

int main() {
    int failures = 0;
    std::div_t d = std::div(17, 5);
    if (d.quot != 3 || d.rem != 2) { printf("FAIL div\n"); failures |= 1; }
    std::ldiv_t l = std::ldiv(-17L, 5L);
    if (l.quot != -3 || l.rem != -2) { printf("FAIL ldiv\n"); failures |= 2; }
    std::lldiv_t ll = std::lldiv(100LL, 7LL);
    if (ll.quot != 14 || ll.rem != 2) { printf("FAIL lldiv\n"); failures |= 4; }
    std::imaxdiv_t m = std::imaxdiv(17, 5);
    if (m.quot != 3 || m.rem != 2) { printf("FAIL imaxdiv\n"); failures |= 8; }
    if (failures == 0) printf("PASS std_full_11_910_div_struct_return\n");
    return failures;
}
