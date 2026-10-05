#include <complex>
#include <cstdio>
int main()
{
    int failures = 0;
    std::complex<double> a(1.0, 2.0);
    std::complex<double> s = a * 2.0;
    std::complex<double> t = 2.0 * a;
    std::complex<double> u = a + 1.0;
    if (s != std::complex<double>(2.0, 4.0) || t != s || u != std::complex<double>(2.0, 2.0) || !(a == std::complex<double>(1.0, 2.0))) { printf("FAIL complex literal operand\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_906_complex_literal_operand\n");
    return failures;
}
