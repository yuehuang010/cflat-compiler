// C++20 equivalent of std_full_11_902_allocator_traits_allocate.cb (compile-time parity baseline)
#include <string>
#include <memory>
#include <cstdio>

int main() {
    int failures = 0;
    std::allocator<int> alloc;
    int* t = std::allocator_traits<std::allocator<int>>::allocate(alloc, 4);
    t[0] = 5; t[3] = 8;
    bool ok = t[0] == 5 && t[3] == 8;
    std::allocator_traits<std::allocator<int>>::deallocate(alloc, t, 4);
    if (!ok) { printf("FAIL traits allocate\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_902_allocator_traits_allocate\n");
    return failures;
}
