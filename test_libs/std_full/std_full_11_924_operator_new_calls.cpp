// C++20 equivalent of std_full_11_924_operator_new_calls.cb (compile-time parity baseline)
#include <string>
#include <new>
#include <cstdint>
#include <cstdio>

int main() {
    int failures = 0;
    void* p = ::operator new(64);
    int* ip = static_cast<int*>(p);
    ip[0] = 11; ip[15] = 22;
    bool alloc_ok = p != nullptr && ip[0] == 11 && ip[15] == 22;
    ::operator delete(p);
    if (!alloc_ok) { printf("FAIL operator new\n"); failures |= 1; }
    void* q = ::operator new(32, std::nothrow);
    bool nothrow_ok = q != nullptr;
    ::operator delete(q, std::nothrow);
    if (!nothrow_ok) { printf("FAIL nothrow new\n"); failures |= 2; }
    void* a = ::operator new(128, std::align_val_t{64});
    bool aligned = (reinterpret_cast<std::uintptr_t>(a) % 64) == 0;
    ::operator delete(a, std::align_val_t{64});
    if (!aligned) { printf("FAIL aligned new\n"); failures |= 4; }
    if (failures == 0) printf("PASS std_full_11_924_operator_new_calls\n");
    return failures;
}
