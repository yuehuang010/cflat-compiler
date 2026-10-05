// C++20 equivalent of std_full_11_920_mem_fn.cb (compile-time parity baseline)
#include <string>
#include <functional>
#include <cstdio>

int main() {
    int failures = 0;
    auto msize = std::mem_fn(&std::string::size);
    auto mempty = std::mem_fn(&std::string::empty);
    std::string str = "hello";
    if (msize(str) != 5 || mempty(str) || !mempty(std::string())) { printf("FAIL mem_fn\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_920_mem_fn\n");
    return failures;
}
