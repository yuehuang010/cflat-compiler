// C++20 equivalent of std_11_numeric_time_random.cb (compile-time parity baseline)
#include <chrono>
#include <cmath>
#include <complex>
#include <limits>
#include <random>
#include <ratio>
#include <type_traits>
#include <cstdio>

int main() {
    int failures = 0;
    using namespace std::chrono;
    milliseconds ms(1500);
    steady_clock::time_point t0 = steady_clock::now(); steady_clock::time_point t1 = steady_clock::now();
    std::mt19937 rng(5489u); unsigned long first = rng();
    std::mt19937_64 rng64(5489u); unsigned long long first64 = rng64();
    std::complex<double> c(1.0, 2.0);
    if (ms.count() != 1500 || !(t1 >= t0) || first != 3499211612UL || first64 != 14514284786278117030ULL || std::ratio<1, 1000>::num != 1 || std::ratio<1, 1000>::den != 1000) { std::printf("FAIL time/random\n"); failures |= 1; }
    if (std::fma(2.0, 3.0, 4.0) != 10.0 || !std::isnan(std::numeric_limits<double>::quiet_NaN()) || std::round(2.4) != 2.0 || std::hypot(3.0, 4.0) != 5.0 || c.real() != 1.0 || c.imag() != 2.0 || !std::is_integral<int>::value || std::is_floating_point<int>::value) { std::printf("FAIL math/types\n"); failures |= 2; }
    if (!failures) std::printf("PASS std_11_numeric_time_random\n");
    return failures;
}
