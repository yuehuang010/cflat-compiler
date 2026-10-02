// C++20 equivalent of std_14_93_is_null_pointer.cb (compile-time parity baseline)
#include <string>
#include <type_traits>
#include <cstdio>
int main() {
    int failures = 0;
    if (!std::is_null_pointer<std::nullptr_t>::value) { std::printf("FAIL is_null_pointer\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_14_93_is_null_pointer\n");
    return failures;
}
