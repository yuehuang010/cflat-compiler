// C++20 equivalent of std_11_95_unique_custom_deleter.cb (compile-time parity baseline)
#include <memory>
#include <cstdio>
#include <string>

int deleted = 0;
void destroy_int(int* p) { delete p; ++deleted; }

int main() {
    int failures = 0;
    { std::unique_ptr<int, void(*)(int*)> p(new int(10), destroy_int); if (*p != 10) { std::printf("FAIL custom deleter value\n"); failures |= 1; } }
    if (deleted != 1) { std::printf("FAIL custom deleter count\n"); failures |= 2; }
    if (!failures) std::printf("PASS std_11_95_unique_custom_deleter\n");
    return failures;
}
