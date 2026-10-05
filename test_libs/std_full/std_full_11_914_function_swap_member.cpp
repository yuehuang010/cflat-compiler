// C++20 equivalent of std_full_11_914_function_swap_member.cb (compile-time parity baseline)
#include <string>
#include <functional>
#include <cstdio>

int twice(int x) { return x * 2; }
int thrice(int x) { return x * 3; }

int main() {
    int failures = 0;
    std::function<int(int)> f1 = twice, f2 = thrice;
    f1.swap(f2);
    if (f1(4) != 12 || f2(4) != 8) { printf("FAIL function::swap\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_914_function_swap_member\n");
    return failures;
}
