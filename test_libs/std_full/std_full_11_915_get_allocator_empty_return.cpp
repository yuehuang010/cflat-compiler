// C++20 equivalent of std_full_11_915_get_allocator_empty_return.cb (compile-time parity baseline)
#include <string>
#include <scoped_allocator>
#include <vector>
#include <memory>
#include <cstdio>

using Alloc = std::scoped_allocator_adaptor<std::allocator<std::string>>;

int main() {
    int failures = 0;
    Alloc a;
    std::vector<std::string, Alloc> v(a);
    Alloc got = v.get_allocator();
    if (got != a || !(got == a)) { printf("FAIL get_allocator compare\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_915_get_allocator_empty_return\n");
    return failures;
}
