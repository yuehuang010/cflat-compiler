// C++20 equivalent of std_full_11_923_offsetof_macro.cb (compile-time parity baseline)
#include <string>
#include <cstddef>
#include <cstdio>

struct Layout { char tag; double value; int count; };

int main() {
    int failures = 0;
    std::size_t off_tag = offsetof(Layout, tag);
    std::size_t off_value = offsetof(Layout, value);
    std::size_t off_count = offsetof(Layout, count);
    if (off_tag != 0 || off_value < 1 || off_value % alignof(double) != 0) { printf("FAIL offsetof\n"); failures |= 1; }
    if (off_count <= off_value) { printf("FAIL offsetof order\n"); failures |= 2; }
    if (failures == 0) printf("PASS std_full_11_923_offsetof_macro\n");
    return failures;
}
