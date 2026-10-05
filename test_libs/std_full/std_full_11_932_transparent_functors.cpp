// C++20 equivalent of std_full_11_932_transparent_functors.cb (compile-time parity baseline)
#include <string>
#include <functional>
#include <cstdio>

int main() {
    int failures = 0;
    std::plus<> plusf; std::less<> ltf; std::multiplies<> mulf; std::greater<> gtf;
    if (plusf(1, 2) != 3 || plusf(1.5, 2.0) != 3.5) { printf("FAIL plus<>\n"); failures |= 1; }
    if (!ltf(1, 2) || ltf(2, 1) || !gtf(std::string("b"), std::string("a"))) { printf("FAIL less<> greater<>\n"); failures |= 2; }
    if (mulf(2, 3.5) != 7.0) { printf("FAIL multiplies<>\n"); failures |= 4; }
    if (std::identity()(5) != 5) { printf("FAIL identity temporary\n"); failures |= 8; }
    if (failures == 0) printf("PASS std_full_11_932_transparent_functors\n");
    return failures;
}
