// C++20 equivalent of std_11_91_duration_cast.cb (compile-time parity baseline)
#include <chrono>
#include <cstdio>
#include <string>

int main() {
    int failures = 0;
    std::chrono::milliseconds ms(1500);
    std::chrono::seconds sec = std::chrono::duration_cast<std::chrono::seconds>(ms);
    if (sec.count() != 1) { std::printf("FAIL duration_cast\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_11_91_duration_cast\n");
    return failures;
}
